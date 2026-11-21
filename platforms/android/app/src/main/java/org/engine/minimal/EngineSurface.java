package org.engine.minimal;

import android.content.Context;
import android.os.Trace;
import android.view.SurfaceHolder;
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
