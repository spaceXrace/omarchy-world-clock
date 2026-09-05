#!/usr/bin/env python3
"""Run the native wallpaper; sleep when Hyprland windows cover >=95% of its output."""
import argparse, json, os, selectors, signal, socket, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def ipc(command):
    path=Path(os.environ['XDG_RUNTIME_DIR'])/'hypr'/os.environ['HYPRLAND_INSTANCE_SIGNATURE']/'.socket.sock'
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
        s.settimeout(2);s.connect(str(path));s.sendall(('j/'+command).encode());parts=[]
        while data:=s.recv(65536):parts.append(data)
    return json.loads(b''.join(parts))
def union_area(rects):
    xs=sorted({v for r in rects for v in (r[0],r[2])});area=0
    for a,b in zip(xs,xs[1:]):
        spans=sorted((r[1],r[3]) for r in rects if r[0]<b and r[2]>a)
        end=-float('inf');height=0
        for lo,hi in spans:
            height+=max(0,hi-max(lo,end));end=max(end,hi)
        area+=(b-a)*height
    return area

def covered(monitor,clients):
    if not monitor.get('dpmsStatus',True):return True
    w,h=monitor['width']/monitor['scale'],monitor['height']/monitor['scale']
    if monitor.get('transform',0)%2:w,h=h,w
    ox,oy=monitor['x'],monitor['y'];rects=[]
    workspaces={monitor['activeWorkspace']['id']}
    if monitor.get('specialWorkspace',{}).get('id',0):workspaces.add(monitor['specialWorkspace']['id'])
    for c in clients:
        if not c.get('mapped') or c.get('hidden') or not c.get('visible',True):continue
        if not c.get('pinned') and c['workspace']['id'] not in workspaces:continue
        x,y=c['at'];cw,ch=c['size'];x0,y0=max(0,x-ox),max(0,y-oy);x1,y1=min(w,x+cw-ox),min(h,y+ch-oy)
        if x1>x0 and y1>y0:rects.append((x0,y0,x1,y1))
    return union_area(rects)/(w*h)>=.95

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',default=None);parser.add_argument('--fps',type=float,default=30)
    parser.add_argument('--binary',default=str(ROOT/'build/cities-earth'))
    parser.add_argument('--always-run',action='store_true',help='Disable coverage pause for measurement')
    args,extra=parser.parse_known_args()
    if not args.output:
        try:
            monitors=ipc('monitors')
            args.output=next((m['name'] for m in monitors if m.get('focused')),monitors[0]['name'] if monitors else None)
        except (OSError, ValueError, KeyError):
            pass
    cmd=[args.binary,'--assets',str(ROOT/'assets'),'--fps',str(args.fps),*extra]
    if args.output: cmd.extend(['--output',args.output])
    child=subprocess.Popen(cmd)
    stop=False
    def shutdown(*_):
        nonlocal stop
        stop=True
    signal.signal(signal.SIGTERM,shutdown);signal.signal(signal.SIGINT,shutdown)
    # Signals interrupt the selector through a nonblocking self-pipe.
    r,w=os.pipe2(os.O_NONBLOCK|os.O_CLOEXEC);signal.set_wakeup_fd(w)
    sel=selectors.DefaultSelector();sel.register(r,selectors.EVENT_READ)
    events=None
    if not args.always_run:
        try:
            events=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
            path=Path(os.environ['XDG_RUNTIME_DIR'])/'hypr'/os.environ['HYPRLAND_INSTANCE_SIGNATURE']/'.socket2.sock'
            events.connect(str(path));events.setblocking(False);sel.register(events,selectors.EVENT_READ)
        except (OSError,KeyError) as e:print(f'Coverage detection unavailable: {e}',flush=True)
    # pidfd wakes immediately on child exit, without periodic process polling.
    pidfd=os.pidfd_open(child.pid);sel.register(pidfd,selectors.EVENT_READ)
    paused=None;pending=time.monotonic()+1;event_buffer=""
    def update():
        nonlocal paused
        try:
            monitors=ipc('monitors')
            monitor=next((m for m in monitors if m['name']==args.output),None) if args.output else next((m for m in monitors if m.get('focused')),monitors[0] if monitors else None)
            state=monitor is None or covered(monitor,ipc('clients'))
            if state!=paused:
                child.send_signal(signal.SIGUSR1 if state else signal.SIGUSR2);paused=state
                print('Paused: output covered/off' if state else 'Animating: desktop visible',flush=True)
        except (OSError,ValueError,KeyError,StopIteration) as e:
            print(f'Coverage check failed; resuming: {e}',flush=True)
            child.send_signal(signal.SIGUSR2);paused=False
    try:
        while not stop and child.poll() is None:
            timeout=max(0,pending-time.monotonic()) if pending else None
            ready=sel.select(timeout)
            for key,_ in ready:
                if key.fileobj==r:os.read(r,4096)
                elif key.fileobj==events:
                    data=events.recv(65536)
                    if not data:
                        sel.unregister(events);events.close();events=None
                        child.send_signal(signal.SIGUSR2);paused=False
                    else:
                        # Ignore title/urgent notifications: do not wake renderer for clock/title changes.
                        relevant=('workspace','openwindow','closewindow','movewindow','changefloatingmode','fullscreen','monitor','configreloaded','activewindow','pin','movelayer','openlayer','closelayer','togglegroup','moveintogroup','moveoutofgroup')
                        event_buffer+=data.decode(errors='replace')
                        lines=event_buffer.split('\n');event_buffer=lines.pop()
                        if any(line.split('>>',1)[0].startswith(relevant) for line in lines):
                            pending=time.monotonic()+.35
            if pending and time.monotonic()>=pending:
                if events is not None:update()
                pending=None
    finally:
        if child.poll() is None:
            child.terminate()
            try:child.wait(timeout=3)
            except subprocess.TimeoutExpired:child.kill();child.wait()
        signal.set_wakeup_fd(-1);sel.close();os.close(r);os.close(w);os.close(pidfd)
        if events:events.close()
    return child.returncode
if __name__=='__main__':raise SystemExit(main())
