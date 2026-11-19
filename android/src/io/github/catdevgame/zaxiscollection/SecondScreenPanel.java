package io.github.catdevgame.zaxiscollection;

import android.app.Presentation;
import android.content.Context;
import android.os.Bundle;
import android.view.Display;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;

import java.io.File;

// The second screen: a Presentation holding one SurfaceView, whose surface goes
// to the native painter (collection/android_app.cpp, about 20 Hz on the CPU, off
// the game's frames and its GPU). Not focusable, so a touch here leaves the pad
// with the game on the top screen and gamepad B cannot fall back to Back and
// cancel the panel; such a window still gets touches.
final class SecondScreenPanel extends Presentation implements SurfaceHolder.Callback {
    // panel::TouchPhase.
    private static final int DOWN = 0, MOVE = 1, UP = 2, CANCEL = 3;

    private static native void nativePanelSurface(android.view.Surface surface, String folder);

    private static native void nativePanelTouch(int phase, int pointer, float x, float y);

    private final SurfaceView mView;

    SecondScreenPanel(Context context, Display display) {
        super(context, display);
        setCancelable(false);
        // Before show(): a focusable panel takes key focus at the first touch.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().preferredRefreshRate = 60.0f;
        mView = new SurfaceView(getContext());
        mView.getHolder().addCallback(this);
        mView.setOnTouchListener((view, event) -> {
            sendTouch(event);
            return true;
        });
    }

    private static void sendTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        switch (action) {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN:
            nativePanelTouch(DOWN, event.getPointerId(index), event.getX(index), event.getY(index));
            break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
            nativePanelTouch(UP, event.getPointerId(index), event.getX(index), event.getY(index));
            break;
        case MotionEvent.ACTION_MOVE:
        case MotionEvent.ACTION_CANCEL:
            for (int i = 0; i < event.getPointerCount(); ++i) {
                nativePanelTouch(action == MotionEvent.ACTION_MOVE ? MOVE : CANCEL, event.getPointerId(i),
                        event.getX(i), event.getY(i));
            }
            break;
        default:
            break;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(mView);
        hideBars();
    }

    // On this window's own decor view, or the display keeps a nav bar inset.
    @SuppressWarnings("deprecation")
    private void hideBars() {
        getWindow().setDecorFitsSystemWindows(false);
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        File folder = new File(getContext().getExternalFilesDir(null), "panel");
        nativePanelSurface(holder.getSurface(), folder.getPath());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativePanelSurface(null, null);
    }
}
