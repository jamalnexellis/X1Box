import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import java.io.ByteArrayOutputStream;
import java.lang.reflect.Method;

/* Shell-only test helper. No APK/controller changes; no root or guest patch. */
public final class DrivingTouch {
    static final String TARGET = "com.izzy2lost.x1box.dev.local/com.izzy2lost.x1box.MainActivity";
    static Object manager;
    static Method inject;
    static long down;
    static int width, height;
    static boolean focused() throws Exception {
        Process p = new ProcessBuilder("dumpsys", "window").start();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        p.getInputStream().transferTo(bytes);
        if (p.waitFor() != 0) return false;
        for (String line : bytes.toString("UTF-8").split("\n")) {
            if (line.contains("mCurrentFocus=")) return line.contains(TARGET);
        }
        return false;
    }
    static void send(int action, int count, float steer) throws Exception {
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[count];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[count];
        for (int i=0;i<count;i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].id = i; props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].pressure = 1; coords[i].size = 1;
            coords[i].x = width * (i == 0 ? .18f + .07f * steer : .946f);
            coords[i].y = i == 0 ? height * .82f : width * .054f;
        }
        MotionEvent e = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action,
            count, props, coords, 0, 0, 1, 1, -1, 0,
            InputDevice.SOURCE_TOUCHSCREEN, 0);
        try {
            if (!((Boolean)inject.invoke(manager, e, 2)))
                throw new IllegalStateException("Touch injection rejected");
        } finally { e.recycle(); }
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 3 && args.length != 4) throw new IllegalArgumentException("width height seconds:steering,...");
        String label=args.length==4?args[3]:null;
        if(label!=null&&!label.matches("perf36-[a-zA-Z0-9_-]+")) throw new IllegalArgumentException("label");
        width=Integer.parseInt(args[0]); height=Integer.parseInt(args[1]);
        if(width<600||width>4000||height<400||height>2400) throw new IllegalArgumentException("geometry");
        String[] phases=args[2].split(","); float[] steering=new float[phases.length];
        boolean[] throttle=new boolean[phases.length];
        long[] duration=new long[phases.length]; long total=0;
        for(int i=0;i<phases.length;i++) {
            String[] pair=phases[i].split(":"); duration[i]=Long.parseLong(pair[0])*1000;
            steering[i]=Float.parseFloat(pair[1]);
            throttle[i]=pair.length<3||pair[2].equals("1");
            if(pair.length==3&&!pair[2].equals("0")&&!pair[2].equals("1")) throw new IllegalArgumentException("throttle");
            if(duration[i]<=0||!Float.isFinite(steering[i])||Math.abs(steering[i])>1) throw new IllegalArgumentException("phase");
            total+=duration[i];
        }
        if(total>60000) throw new IllegalArgumentException("60 second limit");
        Class<?> cls;
        try {cls=Class.forName("android.hardware.input.InputManagerGlobal");}
        catch(ClassNotFoundException missing) {cls=Class.forName("android.hardware.input.InputManager");}
        manager=cls.getMethod("getInstance").invoke(null);
        inject=cls.getMethod("injectInputEvent",InputEvent.class,int.class);
        if(!focused()) throw new IllegalStateException("Test activity not in foreground");
        down=SystemClock.uptimeMillis();
        send(MotionEvent.ACTION_DOWN,1,0);
        boolean pressed=false;
        try {
            for(int i=0;i<phases.length;i++) {
                if(!focused()) throw new IllegalStateException("Test activity left foreground");
                if(throttle[i]!=pressed) {
                    send((throttle[i]?MotionEvent.ACTION_POINTER_DOWN:MotionEvent.ACTION_POINTER_UP)|
                        (1<<MotionEvent.ACTION_POINTER_INDEX_SHIFT),2,steering[i]);
                    pressed=throttle[i];
                }
                if(i==0&&label!=null) Log.i("x1box-bench",label+" touch_begin uptime="+SystemClock.uptimeMillis());
                System.out.println("phase="+i+" uptime="+SystemClock.uptimeMillis()+" steer="+steering[i]+" throttle="+pressed);
                long until=SystemClock.uptimeMillis()+duration[i], check=0;
                while(SystemClock.uptimeMillis()<until) {
                    long now=SystemClock.uptimeMillis();
                    if(now>=check) {
                        if(!focused()) throw new IllegalStateException("Test activity left foreground");
                        check=now+1000;
                    }
                    send(MotionEvent.ACTION_MOVE,pressed?2:1,steering[i]);
                    SystemClock.sleep(50);
                }
            }
        } finally {
            if(focused()) send(MotionEvent.ACTION_CANCEL,pressed?2:1,0);
            long released=SystemClock.uptimeMillis();
            if(label!=null) Log.i("x1box-bench",label+" touch_end uptime="+released);
            System.out.println("released uptime="+released);
        }
    }
}
