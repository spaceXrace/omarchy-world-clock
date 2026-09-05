#define _GNU_SOURCE
#include "fonts.h"
#include "solar.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <math.h>
#include <png.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#define PI 3.14159265358979323846
#define MAX_CITIES 512
#define MAX_VERTICES 80000
#define MAX_STARS 10000
static struct wl_display *display;
static struct wp_viewporter *viewporter;
static struct wp_viewport *viewport;
static int wake_pipe[2] = {-1, -1};
static struct wl_compositor *compositor;
static struct wl_surface *surface;
static struct wl_output *output;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer;
static struct xdg_wm_base *wm;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_egl_window *egl_window;
static struct wl_callback *frame_callback;
static EGLDisplay ed;
static EGLSurface es;
static EGLContext ec;
static volatile sig_atomic_t running = 1, paused = 0;
static int width = 1200, height = 750, configured = 0, frame_ready = 1, preview = 0,
           frame_count = 0, static_mode = 0;
static double fps = 30, render_scale = 1.6, period = 450, duration = 0, start, now, epoch = 0;
static const char *capture = NULL, *output_name = NULL;
static char assets[4096] = "assets";
static GLuint earth_program, sprite_program, earth_vbo, earth_ibo, sprite_vbo, textures[3];
static int atlas_w = 1024, atlas_h = 256;
static int earth_indices;
static float cam, tilt = 15 * PI / 180, cx, cy, radius;
static int indices[MAX_CITIES], history_count = 0;
static double history_born[MAX_CITIES];
/* Continuous drift is applied equally to every displayed and target position.
   New arrivals shift every older target by the same amount, and a common
   capped velocity closes the gap without row-specific easing rhythms. */
static float list_y[MAX_CITIES], list_target[MAX_CITIES];
static double list_prev_now = -1;
static int list_prev_h = 0;
typedef struct {
    char *name, *country;
    double population, lat, lon, born, previous_position;
    GTimeZone *tz;
} City;
static City cities[MAX_CITIES];
static int city_count;
typedef struct {
    float ra, dec, magnitude, r, g, b, alpha;
} Star;
static Star stars[MAX_STARS];
static int star_count;
typedef struct {
    float x, y, u, v, r, g, b, a, sharpening;
} Sprite;
static Sprite sprites[MAX_VERTICES];
static int sprite_count;
static double monotime(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void die(const char *s) {
    fprintf(stderr, "cities-earth: %s\n", s);
    exit(1);
}
static float clampf(float x, float a, float b) {
    return fminf(b, fmaxf(a, x));
}
static void signal_handler(int s) {
    if (s == SIGUSR1)
        paused = 1;
    else if (s == SIGUSR2)
        paused = 0;
    else
        running = 0;
    int saved = errno;
    if (wake_pipe[1] >= 0) {
        char c = 1;
        ssize_t ignored = write(wake_pipe[1], &c, 1);
        (void)ignored;
    }
    errno = saved;
}
static GLuint shader(GLenum type, const char *source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        die(log);
    }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(), v = shader(GL_VERTEX_SHADER, vs),
           f = shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "position");
    glBindAttribLocation(p, 1, "uv");
    glBindAttribLocation(p, 2, "color");
    glBindAttribLocation(p, 3, "sharpening");
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
        die("shader link failed");
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}
static GLuint texture(const char *name) {
    char path[8192];
    snprintf(path, sizeof path, "%s/%s", assets, name);
    png_image im = {.version = PNG_IMAGE_VERSION};
    if (!png_image_begin_read_from_file(&im, path))
        die(path);
    im.format = PNG_FORMAT_RGBA;
    void *pixels = malloc(PNG_IMAGE_SIZE(im));
    if (!pixels || !png_image_finish_read(&im, NULL, pixels, 0, NULL))
        die("PNG decode failed");
    if (!strcmp(name, "atlas.png")) {
        atlas_w = im.width;
        atlas_h = im.height;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im.width, im.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    /* The 8K Earth maps contain detail far below one screen pixel, especially
       isolated night lights. Trilinear mip filtering keeps that detail from
       flickering as the globe rotates. It also preserves the atlas's smooth
       minification for bitmap glyphs and point sprites. */
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    strcmp(name, "atlas.png") == 0 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(pixels);
    png_image_free(&im);
    return tex;
}
static void setup_gl(void) {
    earth_program = program(
        "attribute vec3 position;attribute vec2 uv;uniform vec2 screen,center;uniform float "
        "radius,cam,tilt;uniform vec3 sun;varying vec2 texcoord;varying float daylight;varying "
        "float depth;void main(){float x=position.x*cos(cam)-position.z*sin(cam);float "
        "z=position.x*sin(cam)+position.z*cos(cam);float "
        "y=position.y*cos(tilt)-z*sin(tilt);z=position.y*sin(tilt)+z*cos(tilt);vec2 "
        "p=center+radius*vec2(x,-y);gl_Position=vec4(p.x/screen.x*2.0-1.0,1.0-p.y/"
        "screen.y*2.0,-z*.5,1.0);texcoord=uv;daylight=dot(position,sun);depth=z;}",
        "precision mediump float;uniform sampler2D daymap,nightmap;varying vec2 texcoord;varying "
        "float daylight;varying float depth;void main(){if(depth<0.0)discard;vec3 "
        "d=texture2D(daymap,texcoord).rgb;vec3 n=texture2D(nightmap,texcoord).rgb;float "
        "lum=dot(n,vec3(.299,.587,.114));float "
        "lights=pow(clamp(lum,0.0,1.0),1.5)*(.35+.65*smoothstep(-.02,.10,n.r-n.b));vec3 "
        "nightbase=n*(1.0-lights);vec3 lightcol=n*lights*1.6;float "
        "dayW=smoothstep(-.12,.40,daylight);float lightAmt=1.0-smoothstep(.45,.55,daylight);"
        "float basedim=mix(.30,.72,smoothstep(-.55,-.02,daylight));vec3 "
        "c=d*dayW+nightbase*basedim*(1.0-dayW)+lightcol*lightAmt*(1.0-dayW*.85);"
        "gl_FragColor=vec4(c*(.8+.2*max(depth,0.0)),1.0);}");
    sprite_program = program(
        "attribute vec2 position,uv;attribute vec4 color;attribute float sharpening;"
        "uniform vec2 screen;varying vec2 texcoord;varying vec4 tint;varying float "
        "sharpness;void "
        "main(){gl_Position=vec4(position.x/screen.x*2.0-1.0,1.0-position.y/"
        "screen.y*2.0,0.0,1.0);texcoord=uv;tint=color;sharpness=sharpening;}",
        "precision mediump float;uniform sampler2D atlas;varying vec2 texcoord;varying "
        "vec4 tint;varying float sharpness;void main(){vec4 sample=texture2D(atlas,texcoord);float "
        "isFont=1.0-step(.75,texcoord.x);float crisp=smoothstep(.12,.82,sample.a);float "
        "alpha=mix(sample.a,crisp,isFont*sharpness);gl_FragColor=vec4(sample.rgb*tint.rgb,"
        "alpha*tint.a);}");
    enum { NX = 128, NY = 64 };
    float vertices[(NX + 1) * (NY + 1) * 5];
    unsigned short tris[NX * NY * 6];
    int v = 0, t = 0;
    for (int y = 0; y <= NY; y++) {
        double lat = PI / 2 - PI * y / NY;
        for (int x = 0; x <= NX; x++) {
            double lon = 2 * PI * x / NX;
            vertices[v++] = cos(lat) * sin(lon);
            vertices[v++] = sin(lat);
            vertices[v++] = cos(lat) * cos(lon);
            vertices[v++] = (float)x / NX;
            vertices[v++] = (float)y / NY;
        }
    }
    for (int y = 0; y < NY; y++)
        for (int x = 0; x < NX; x++) {
            unsigned short a = y * (NX + 1) + x, b = a + NX + 1;
            tris[t++] = a;
            tris[t++] = b;
            tris[t++] = a + 1;
            tris[t++] = a + 1;
            tris[t++] = b;
            tris[t++] = b + 1;
        }
    earth_indices = t;
    glGenBuffers(1, &earth_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, earth_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    glGenBuffers(1, &earth_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, earth_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof tris, tris, GL_STATIC_DRAW);
    glGenBuffers(1, &sprite_vbo);
    textures[0] = texture("8k_earth_daymap.png");
    textures[1] = texture("8k_earth_nightmap.png");
    textures[2] = texture("atlas.png");
    fprintf(stderr, "GPU: %s / %s; %.1f fps, buffer scale %.2f\n", glGetString(GL_VENDOR),
            glGetString(GL_RENDERER), fps, render_scale);
}
static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 float r, float g, float b, float a, float sharpening) {
    if (sprite_count + 6 > MAX_VERTICES)
        die("sprite capacity exceeded");
    Sprite q[4] = {{x, y, u0, v0, r, g, b, a, sharpening},
                   {x + w, y, u1, v0, r, g, b, a, sharpening},
                   {x + w, y + h, u1, v1, r, g, b, a, sharpening},
                   {x, y + h, u0, v1, r, g, b, a, sharpening}};
    int order[] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++)
        sprites[sprite_count++] = q[order[i]];
}
static float text_width(int font, const char *s, float size) {
    float result = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        result += (glyphs[font][*p][4] + 1) * size / font_heights[font];
    return result;
}
static void text(int font, const char *s, float x, float y, float size, float r, float g, float b,
                 float a) {
    float scale = size / font_heights[font];
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const float *v = glyphs[font][*p];
        if (v[2] > v[0])
            quad(x, y, (v[2] - v[0]) * atlas_w * scale, (v[3] - v[1]) * atlas_h * scale, v[0], v[1],
                 v[2], v[3], r, g, b, a, font == 1 ? .45f : 1.f);
        x += (v[4] + 1) * scale;
    }
}
static void glow(float x, float y, float size, float r, float g, float b, float a) {
    quad(x - size, y - size, size * 2, size * 2, .7505, .002, .8745, .498, r, g, b, a, 0);
}
static int project(City *c, float *x, float *y) {
    double xx = cos(c->lat) * sin(c->lon - cam), z = cos(c->lat) * cos(c->lon - cam),
           yy = sin(c->lat) * cos(tilt) - z * sin(tilt);
    z = sin(c->lat) * sin(tilt) + z * cos(tilt);
    *x = cx + radius * xx;
    *y = cy - radius * yy;
    return z > .12;
}
static void load_cities(void) {
    char path[8192];
    snprintf(path, sizeof path, "%s/cities.tsv", assets);
    FILE *f = fopen(path, "r");
    if (!f)
        die("assets/cities.tsv missing; run make assets");
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        gchar **p = g_strsplit(line, "\t", -1);
        if (g_strv_length(p) < 6 || city_count >= MAX_CITIES)
            die("Invalid city table");
        City *c = &cities[city_count++];
        c->name = g_strdup(p[0]);
        c->country = g_strdup(p[1]);
        c->population = g_ascii_strtod(p[2], NULL);
        c->lat = g_ascii_strtod(p[3], NULL) * PI / 180;
        c->lon = g_ascii_strtod(p[4], NULL) * PI / 180;
        c->tz = g_time_zone_new_identifier(p[5]);
        c->born = -1;
        c->previous_position = NAN;
        if (!c->tz)
            die("Invalid IANA timezone");
        g_strfreev(p);
    }
    free(line);
    fclose(f);
    if (!city_count)
        die("Empty city table");
}
static void load_stars(void) {
    /* Deterministic synthetic sky: redistributable and stable across launches. */
    unsigned int state = 0x5eed1234u;
    for (int i = 0; i < 1729; i++) {
        state = state * 1664525u + 1013904223u;
        Star *s = &stars[star_count++];
        s->ra = (state >> 8) * (360.f / 16777216.f);
        state = state * 1664525u + 1013904223u;
        s->dec = (state >> 8) * (180.f / 16777216.f) - 90.f;
        state = state * 1664525u + 1013904223u;
        s->magnitude = .2f + (state >> 8) * (2.3f / 16777216.f);
        state = state * 1664525u + 1013904223u;
        int bv = (int)((state >> 24) * (185.f / 255.f)) - 35;
        if (bv < -35) {
            s->r = .7f;
            s->g = .8f;
            s->b = 1;
        } else if (bv < 15) {
            float t = (bv + 35) / 50.f;
            s->r = .7f + .3f * t;
            s->g = .8f + .2f * t;
            s->b = 1;
        } else if (bv < 60) {
            float t = (bv - 15) / 45.f;
            s->r = 1;
            s->g = 1;
            s->b = 1 - .3f * t;
        } else if (bv < 135) {
            float t = (bv - 60) / 75.f;
            s->r = 1;
            s->g = 1 - .2f * t;
            s->b = .7f;
        } else {
            s->r = 1;
            s->g = .8f;
            s->b = .7f;
        }
        s->alpha = fminf(1, powf(1.2f, 3 - s->magnitude * 2));
    }
}
static void clock_text(GDateTime *utc, GTimeZone *tz, char out[16]) {
    GDateTime *local = g_date_time_to_timezone(utc, tz);
    snprintf(out, 16, "%02d:%02d:%02d", g_date_time_get_hour(local), g_date_time_get_minute(local),
             g_date_time_get_second(local));
    g_date_time_unref(local);
}
static void scene(void) {
    now = monotime() - start;
    cam = fmod(20 * PI / 180 + now * 2 * PI / period, 2 * PI);
    cx = width * .5;
    cy = height * .51;
    radius = fmin(height * .445, width * .335);
    /* Longitude position makes the same relative boundary work at every latitude. */
    for (int i = 0; i < city_count; i++) {
        City *c = &cities[i];
        double longitude_position = sin(c->lon - cam);
        double facing = cos(c->lon - cam);
        if (isnan(c->previous_position)) {
            c->previous_position = longitude_position;
            continue;
        }
        if (c->born < 0 && facing > 0 && c->previous_position > .6 && longitude_position <= .6) {
            c->born = now;
            if (history_count > 0) {
                float top_now = height * .065f;
                float step_now = 108 * height / 1000.f;
                float shift = fmaxf(0, top_now + step_now - list_target[0]);
                for (int k = 0; k < history_count; k++)
                    list_target[k] += shift;
            }
            if (history_count == MAX_CITIES)
                history_count--;
            memmove(indices + 1, indices, history_count * sizeof(int));
            memmove(history_born + 1, history_born, history_count * sizeof(double));
            memmove(list_y + 1, list_y, history_count * sizeof(float));
            memmove(list_target + 1, list_target, history_count * sizeof(float));
            indices[0] = i;
            history_born[0] = now;
            list_y[0] = list_target[0] = height * .065f;
            history_count++;
        } else if (c->born >= 0 && (facing <= 0 || longitude_position < -.6)) {
            c->born = -1;
        }
        c->previous_position = longitude_position;
    }
    int bw = lround(width * render_scale), bh = lround(height * render_scale);
    glViewport(0, 0, bw, bh);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(earth_program);
    glUniform2f(glGetUniformLocation(earth_program, "screen"), width, height);
    glUniform2f(glGetUniformLocation(earth_program, "center"), cx, cy);
    glUniform1f(glGetUniformLocation(earth_program, "radius"), radius);
    glUniform1f(glGetUniformLocation(earth_program, "cam"), cam);
    glUniform1f(glGetUniformLocation(earth_program, "tilt"), tilt);
    time_t timestamp = epoch ? epoch + (time_t)now : time(NULL);
    float sun[3];
    earth_sun_vector(timestamp, sun);
    glUniform3fv(glGetUniformLocation(earth_program, "sun"), 1, sun);
    for (int i = 0; i < 2; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
    }
    glUniform1i(glGetUniformLocation(earth_program, "daymap"), 0);
    glUniform1i(glGetUniformLocation(earth_program, "nightmap"), 1);
    glBindBuffer(GL_ARRAY_BUFFER, earth_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, earth_ibo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glDrawElements(GL_TRIANGLES, earth_indices, GL_UNSIGNED_SHORT, 0);
    sprite_count = 0;
    float scale = height / 1000.f;
    /* The original GLScene sky dome uses this astronomical point catalogue.
       Equirectangular screen projection gives its rotation a seamless horizontal wrap. */
    float star_offset = fmodf(now * width / 450.f, width);
    for (int i = 0; i < star_count; i++) {
        Star *s = &stars[i];
        /* Keep the brighter fifth of the original catalogue. The full naked-eye
           data set is overpowering at wallpaper scale. */
        if (s->magnitude > 2.5f)
            continue;
        float x = fmodf(s->ra / 360.f * width + star_offset, width);
        float y = (90 - s->dec) / 180.f * height;
        if (hypotf(x - cx, y - cy) < radius + 3)
            continue;
        float point_size = fmaxf(1.25f, 3.75f - s->magnitude) * scale;
        quad(x - point_size * .5f, y - point_size * .5f, point_size, point_size, .8755, .002,
             .90575, .123, s->r, s->g, s->b, s->alpha, 0);
    }
    for (int i = 0; i < city_count; i++) {
        City *c = &cities[i];
        if (c->born < 0)
            continue;
        float x, y;
        if (!project(c, &x, &y))
            continue;
        double age = now - c->born;
        glow(x, y, 6.0 * scale, 1, .55, .06, 1);
        if (age < 3) {
            float fade = clampf(age / .25, 0, 1) * clampf((3 - age) / 1.0, 0, 1);
            glow(x, y, 25 * scale, 1, .58, .11, fade);
            glow(x, y, 8 * scale, 1, .92, .65, fade);
        }
        if (age < 10) {
            float a = clampf(age / .4, 0, 1) * clampf((10 - age) / 2, 0, 1);
            float size = 22 * scale;
            float tx = x + 13 * scale;
            text(0, c->name, tx, y - 13 * scale, size, 1, 1, 1, a);
        }
    }
    GDateTime *utc = g_date_time_new_from_unix_utc(timestamp);
    float left = width * .82;
    left = roundf(left);
    float roll_speed = 21 * scale;
    double dt = list_prev_now < 0 ? 0 : fmin(now - list_prev_now, 0.25);
    list_prev_now = now;
    if (list_prev_h > 0 && list_prev_h != height) {
        float ratio = (float)height / list_prev_h;
        for (int k = 0; k < history_count; k++) {
            list_y[k] *= ratio;
            list_target[k] *= ratio;
        }
    }
    list_prev_h = height;
    float drift = roll_speed * dt;
    float push_limit = 180 * scale * dt;
    for (int k = 0; k < history_count; k++) {
        list_y[k] += drift;
        list_target[k] += drift;
        list_y[k] += fminf(fmaxf(0, list_target[k] - list_y[k]), push_limit);
    }
    for (int k = 0; k < history_count && k < 12; k++) {
        City *c = &cities[indices[k]];
        double list_age = fmax(0, now - history_born[k]);
        float y = list_y[k];
        float a = clampf(list_age / 1.0, 0, 1);
        if (y > height - 115 * scale)
            continue;
        if (a <= 0)
            continue;
        char country[160], pop[80], clock[16];
        snprintf(country, sizeof country, "(%s)", c->country);
        snprintf(pop, sizeof pop, "Population: %.1f million", c->population);
        char *dot = strchr(pop, '.');
        if (dot)
            *dot = ',';
        clock_text(utc, c->tz, clock);
        float widest = text_width(1, c->name, 1);
        widest = fmaxf(widest, text_width(1, country, 1));
        widest = fmaxf(widest, text_width(1, pop, 1));
        widest = fmaxf(widest, text_width(1, clock, 1));
        float size = fminf(24 * scale, (width - left - 12 * scale) / fmaxf(1, widest));
        const float green_r = .55, green_g = .86, green_b = .30;
        text(1, c->name, left, y, size, green_r, green_g, green_b,
             a * clampf((height - 115 * scale - y - size) / (100 * scale), 0, 1));
        text(1, country, left, y + 26 * scale, size, green_r, green_g, green_b,
             a * clampf((height - 162 * scale - y) / (100 * scale), 0, 1));
        text(1, pop, left, y + 52 * scale, size, green_r, green_g, green_b,
             a * clampf((height - 185 * scale - y) / (100 * scale), 0, 1));
        text(1, clock, left, y + 78 * scale, size, green_r, green_g, green_b,
             a * clampf((height - 212 * scale - y) / (100 * scale), 0, 1));
    }
    char clock[16];
    GTimeZone *local = g_time_zone_new_local();
    clock_text(utc, local, clock);
    g_time_zone_unref(local);
    g_date_time_unref(utc);
    float size = 38 * scale;
    float clock_x = roundf(width - text_width(2, clock, size) - 24 * scale);
    text(2, clock, clock_x, roundf(height - 58 * scale), size, .83, .88, .96, 1);
    glUseProgram(sprite_program);
    glUniform2f(glGetUniformLocation(sprite_program, "screen"), width, height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures[2]);
    glUniform1i(glGetUniformLocation(sprite_program, "atlas"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, sprite_vbo);
    glBufferData(GL_ARRAY_BUFFER, sprite_count * sizeof(Sprite), sprites, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Sprite), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Sprite), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Sprite), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Sprite), (void *)(8 * sizeof(float)));
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, sprite_count);
    frame_count++;
}
static void screenshot(void) {
    int w = lround(width * render_scale), h = lround(height * render_scale);
    unsigned char *p = malloc(w * h * 4), *flip = malloc(w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p);
    for (int y = 0; y < h; y++)
        memcpy(flip + y * w * 4, p + (h - 1 - y) * w * 4, w * 4);
    png_image im = {
        .version = PNG_IMAGE_VERSION, .width = w, .height = h, .format = PNG_FORMAT_RGBA};
    if (!png_image_write_to_file(&im, capture, 0, flip, 0, NULL))
        die(im.message);
    free(p);
    free(flip);
    fprintf(stderr, "Saved %s\n", capture);
}
static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)data;
    (void)time;
    wl_callback_destroy(cb);
    frame_callback = NULL;
    frame_ready = 1;
}
static const struct wl_callback_listener frame_listener = {frame_done};
static void resize(int w, int h) {
    if (w > 0)
        width = w;
    if (h > 0)
        height = h;
    if (viewport)
        wp_viewport_set_destination(viewport, width, height);
    if (egl_window)
        wl_egl_window_resize(egl_window, lround(width * render_scale),
                             lround(height * render_scale), 0, 0);
}
static void layer_configure(void *data, struct zwlr_layer_surface_v1 *s, uint32_t serial,
                            uint32_t w, uint32_t h) {
    (void)data;
    zwlr_layer_surface_v1_ack_configure(s, serial);
    resize(w, h);
    configured = 1;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *s) {
    (void)data;
    (void)s;
    running = 0;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {layer_configure, layer_closed};
static void wm_ping(void *data, struct xdg_wm_base *s, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(s, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};
static void xdg_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}
static const struct xdg_surface_listener xdg_listener = {xdg_configure};
static void top_configure(void *data, struct xdg_toplevel *s, int32_t w, int32_t h,
                          struct wl_array *states) {
    (void)data;
    (void)s;
    (void)states;
    resize(w, h);
}
static void top_close(void *data, struct xdg_toplevel *s) {
    (void)data;
    (void)s;
    running = 0;
}
static const struct xdg_toplevel_listener top_listener = {.configure = top_configure,
                                                          .close = top_close};
static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char *make, const char *model, int32_t transform) {
    (void)d;
    (void)o;
    (void)x;
    (void)y;
    (void)pw;
    (void)ph;
    (void)sub;
    (void)make;
    (void)model;
    (void)transform;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h,
                     int32_t refresh) {
    (void)d;
    (void)o;
    (void)f;
    (void)w;
    (void)h;
    (void)refresh;
}
static void out_done(void *d, struct wl_output *o) {
    (void)d;
    (void)o;
}
static void out_scale(void *d, struct wl_output *o, int32_t s) {
    (void)d;
    (void)o;
    (void)s;
}
static void out_name(void *d, struct wl_output *o, const char *name) {
    (void)d;
    if (output_name && strcmp(output_name, name) == 0)
        output = o;
}
static void out_description(void *d, struct wl_output *o, const char *s) {
    (void)d;
    (void)o;
    (void)s;
}
static const struct wl_output_listener out_listener = {out_geometry, out_mode, out_done,
                                                       out_scale,    out_name, out_description};
static void registry_global(void *data, struct wl_registry *r, uint32_t id, const char *interface,
                            uint32_t version) {
    (void)data;
    if (!strcmp(interface, "wp_viewporter"))
        viewporter = wl_registry_bind(r, id, &wp_viewporter_interface, 1);
    else if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "zwlr_layer_shell_v1"))
        layer_shell = wl_registry_bind(r, id, &zwlr_layer_shell_v1_interface, 1);
    else if (!strcmp(interface, "xdg_wm_base")) {
        wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    } else if (!strcmp(interface, "wl_output") && output_name && version >= 4) {
        struct wl_output *o = wl_registry_bind(r, id, &wl_output_interface, 4);
        wl_output_add_listener(o, &out_listener, NULL);
    }
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t id) {
    (void)d;
    (void)r;
    (void)id;
}
static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};
int main(int argc, char **argv) {
    pid_t supervisor = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
        die("Cannot monitor wallpaper supervisor");
    if (getppid() != supervisor)
        return 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--preview"))
            preview = 1;
        else if (!strcmp(argv[i], "--static"))
            static_mode = 1;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            render_scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--period") && i + 1 < argc)
            period = atof(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
            duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "--epoch") && i + 1 < argc)
            epoch = atof(argv[++i]);
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc)
            capture = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            output_name = argv[++i];
        else if (!strcmp(argv[i], "--assets") && i + 1 < argc)
            snprintf(assets, sizeof assets, "%s", argv[++i]);
        else {
            fprintf(
                stderr,
                "Usage: %s [--preview] [--static] [--fps 30] [--scale 1] [--period 450] [--output "
                "eDP-1] [--assets DIR] [--duration SEC] [--capture FILE.png] [--epoch UNIX]\n",
                argv[0]);
            return strcmp(argv[i], "--help") ? 1 : 0;
        }
    }
    if (!isfinite(fps) || !isfinite(render_scale) || !isfinite(period) || fps < .2 || fps > 60 ||
        render_scale < .25 || render_scale > 3 || period < 60 || duration < 0 ||
        !isfinite(duration) || !isfinite(epoch))
        die("Invalid numeric option");
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK))
        die("Cannot create signal pipe");
    struct sigaction sa = {.sa_handler = signal_handler};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    load_cities();
    load_stars();
    display = wl_display_connect(NULL);
    if (!display)
        die("Cannot connect to Wayland");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!compositor)
        die("Missing compositor");
    if (output_name && !output)
        die("Requested output not found");
    surface = wl_compositor_create_surface(compositor);
    if (preview) {
        if (!wm)
            die("Missing xdg-shell");
        xdg_surface = xdg_wm_base_get_xdg_surface(wm, surface);
        xdg_surface_add_listener(xdg_surface, &xdg_listener, NULL);
        toplevel = xdg_surface_get_toplevel(xdg_surface);
        xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
        xdg_toplevel_set_title(toplevel, "Cities of Earth — native preview");
        xdg_toplevel_set_app_id(toplevel, "cities-earth");
    } else {
        if (!layer_shell)
            die("Missing layer-shell");
        layer = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell, surface, output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "cities-earth");
        zwlr_layer_surface_v1_set_anchor(layer, 15);
        zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(layer, 0);
        zwlr_layer_surface_v1_add_listener(layer, &layer_listener, NULL);
        struct wl_region *empty = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(surface, empty);
        wl_region_destroy(empty);
    }
    wl_surface_commit(surface);
    while (!configured && running)
        if (wl_display_dispatch(display) < 0)
            die("Configure failed");
    if (!running)
        return 0;
    ed = eglGetDisplay((EGLNativeDisplayType)display);
    if (!eglInitialize(ed, NULL, NULL))
        die("EGL initialization failed");
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint attrs[] = {EGL_SURFACE_TYPE,
                      EGL_WINDOW_BIT,
                      EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_ALPHA_SIZE,
                      0,
                      EGL_DEPTH_SIZE,
                      0,
                      EGL_NONE};
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(ed, attrs, &config, 1, &count) || !count)
        die("No EGL config");
    EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    ec = eglCreateContext(ed, config, EGL_NO_CONTEXT, ctx);
    egl_window =
        wl_egl_window_create(surface, lround(width * render_scale), lround(height * render_scale));
    es = eglCreateWindowSurface(ed, config, (EGLNativeWindowType)egl_window, NULL);
    if (!eglMakeCurrent(ed, es, es, ec))
        die("Cannot make EGL current");
    eglSwapInterval(ed, 0);
    if (render_scale != 1) {
        if (!viewporter)
            die("Compositor lacks wp_viewporter");
        viewport = wp_viewporter_get_viewport(viewporter, surface);
        wp_viewport_set_destination(viewport, width, height);
    }
    struct wl_region *opaque = wl_compositor_create_region(compositor);
    wl_region_add(opaque, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region(surface, opaque);
    wl_region_destroy(opaque);
    setup_gl();
    start = monotime();
    double deadline = start;
    int fd = wl_display_get_fd(display);
    while (running) {
        if (wl_display_dispatch_pending(display) < 0)
            break;
        double t = monotime();
        if (duration && t - start >= duration)
            break;
        if (!paused && frame_ready && t >= deadline) {
            scene();
            if (capture && duration && now + 1 / fps >= duration) {
                screenshot();
                capture = NULL;
            }
            frame_ready = 0;
            frame_callback = wl_surface_frame(surface);
            wl_callback_add_listener(frame_callback, &frame_listener, NULL);
            if (!eglSwapBuffers(ed, es))
                die("EGL swap failed");
            deadline = monotime() + 1 / fps;
            if (static_mode)
                paused = 1;
        }
        while (wl_display_prepare_read(display) != 0)
            if (wl_display_dispatch_pending(display) < 0)
                goto done;
        wl_display_flush(display);
        int timeout = -1;
        t = monotime();
        if (!paused && frame_ready)
            timeout = (int)fmax(0, ceil((deadline - t) * 1000));
        if (duration) {
            int left = fmax(0, ceil((start + duration - t) * 1000));
            if (timeout < 0 || left < timeout)
                timeout = left;
        }
        struct pollfd p[2] = {{fd, POLLIN, 0}, {wake_pipe[0], POLLIN, 0}};
        int result = poll(p, 2, timeout);
        if (result > 0 && (p[1].revents & POLLIN)) {
            char buf[64];
            while (read(wake_pipe[0], buf, sizeof buf) > 0) {
            }
        }
        if (result > 0 && (p[0].revents & POLLIN)) {
            if (wl_display_read_events(display) < 0)
                break;
        } else
            wl_display_cancel_read(display);
        if (result > 0 && (p[0].revents & (POLLERR | POLLHUP)))
            break;
        if (result < 0 && errno != EINTR)
            break;
    }
done:
    if (capture) {
        scene();
        screenshot();
    }
    fprintf(stderr, "Rendered %d frames in %.1fs\n", frame_count, monotime() - start);
    if (frame_callback)
        wl_callback_destroy(frame_callback);
    eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(ed, es);
    eglDestroyContext(ed, ec);
    eglTerminate(ed);
    wl_egl_window_destroy(egl_window);
    if (layer)
        zwlr_layer_surface_v1_destroy(layer);
    if (toplevel)
        xdg_toplevel_destroy(toplevel);
    if (xdg_surface)
        xdg_surface_destroy(xdg_surface);
    if (viewport)
        wp_viewport_destroy(viewport);
    wl_surface_destroy(surface);
    wl_display_disconnect(display);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    return 0;
}
