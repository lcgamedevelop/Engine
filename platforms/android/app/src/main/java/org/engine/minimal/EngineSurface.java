package org.engine.minimal;

import android.content.Context;
import android.graphics.Insets;
import android.os.Build;
import android.os.Trace;
import android.view.DisplayCutout;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowInsets;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

final class EngineSurface extends SDLSurface
{
    EngineSurface(Context context)
    {
        super(context);
    }

    boolean isReadyForRendering()
    {
        Trace.beginSection("Engine.Android.EngineSurface.isReadyForRendering");
        try
        {
            return mIsSurfaceReady;
        }
        finally
        {
            Trace.endSection();
        }
    }

    @Override public WindowInsets onApplyWindowInsets(View view, WindowInsets insets)
    {
        // SDL 3.4 handles the typed insets API on Android 11 and later.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
        {
            int left = insets.getSystemWindowInsetLeft();
            int top = insets.getSystemWindowInsetTop();
            int right = insets.getSystemWindowInsetRight();
            int bottom = insets.getSystemWindowInsetBottom();
            DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null)
            {
                left = Math.max(left, cutout.getSafeInsetLeft());
                top = Math.max(top, cutout.getSafeInsetTop());
                right = Math.max(right, cutout.getSafeInsetRight());
                bottom = Math.max(bottom, cutout.getSafeInsetBottom());
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            {
                Insets gestures = insets.getSystemGestureInsets();
                left = Math.max(left, gestures.left);
                top = Math.max(top, gestures.top);
                right = Math.max(right, gestures.right);
                bottom = Math.max(bottom, gestures.bottom);
            }
            SDLActivity.onNativeInsetsChanged(left, right, top, bottom);
        }
        return super.onApplyWindowInsets(view, insets);
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder)
    {
        Trace.beginSection("Engine.Android.EngineSurface.surfaceDestroyed");
        try
        {
            // Surface 可在 Activity 仍存活时重建；此边界也必须等 bgfx 完全停下。
            EngineActivity.pauseRendering();
            super.surfaceDestroyed(holder);
        }
        finally
        {
            Trace.endSection();
        }
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height)
    {
        Trace.beginSection("Engine.Android.EngineSurface.surfaceChanged");
        try
        {
            super.surfaceChanged(holder, format, width, height);
            EngineActivity.resumeRenderingIfReady();
        }
        finally
        {
            Trace.endSection();
        }
    }
}
