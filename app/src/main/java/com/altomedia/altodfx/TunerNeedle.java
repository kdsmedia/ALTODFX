package com.altomedia.altodfx;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.View;

import androidx.annotation.Nullable;

/**
 * Semi-circular tuning gauge. The needle points at the current deviation from
 * the nearest note, with a green "in tune" band in the centre.
 */
public class TunerNeedle extends View {

    private float cents = 0f;
    private boolean active = false;

    private final Paint arcPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint tickPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint needlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hubPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF arcRect = new RectF();
    private final Path needle = new Path();

    public TunerNeedle(Context context) {
        super(context);
        init();
    }

    public TunerNeedle(Context context, @Nullable AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public TunerNeedle(Context context, @Nullable AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init();
    }

    private void init() {
        arcPaint.setStyle(Paint.Style.STROKE);
        arcPaint.setStrokeCap(Paint.Cap.ROUND);
        tickPaint.setStyle(Paint.Style.STROKE);
        tickPaint.setStrokeWidth(3f);
        needlePaint.setStyle(Paint.Style.FILL);
        needlePaint.setColor(Color.WHITE);
        hubPaint.setStyle(Paint.Style.FILL);
        hubPaint.setColor(Color.WHITE);
    }

    public void setCents(float cents) {
        this.cents = Math.max(-50f, Math.min(50f, cents));
        invalidate();
    }

    public void setActive(boolean active) {
        this.active = active;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        float w = getWidth();
        float h = getHeight();
        float cx = w / 2f;
        float cy = h - 20f;
        float radius = Math.min(w / 2f, h) - 30f;
        if (radius <= 0) return;

        // Background arc: red -> green -> red across -50..+50 cents.
        arcRect.set(cx - radius, cy - radius, cx + radius, cy + radius);
        arcPaint.setStrokeWidth(18f);
        arcPaint.setColor(Color.parseColor("#55FF5252"));
        canvas.drawArc(arcRect, 200f, 50f, false, arcPaint);
        arcPaint.setColor(Color.parseColor("#5533CC55"));
        canvas.drawArc(arcRect, 250f, 40f, false, arcPaint);
        arcPaint.setColor(Color.parseColor("#55FF5252"));
        canvas.drawArc(arcRect, 290f, 50f, false, arcPaint);

        // Centre mark.
        tickPaint.setColor(Color.WHITE);
        double midRad = Math.toRadians(270);
        float midX = cx + (float) (Math.cos(midRad) * (radius - 14));
        float midY = cy + (float) (Math.sin(midRad) * (radius - 14));
        canvas.drawLine(cx, cy, midX, midY, tickPaint);

        // Needle.
        double angle = Math.toRadians(270 + (cents / 50f) * 45f);
        float needleLen = radius - 24;
        float tipX = cx + (float) (Math.cos(angle) * needleLen);
        float tipY = cy + (float) (Math.sin(angle) * needleLen);
        needle.reset();
        double perp = angle + Math.PI / 2;
        float baseHalf = 8f;
        needle.moveTo(
                cx + (float) (Math.cos(perp) * baseHalf),
                cy + (float) (Math.sin(perp) * baseHalf));
        needle.lineTo(tipX, tipY);
        needle.lineTo(
                cx - (float) (Math.cos(perp) * baseHalf),
                cy - (float) (Math.sin(perp) * baseHalf));
        needle.close();
        needlePaint.setColor(active ? Color.parseColor("#FFEB3B") : Color.parseColor("#99FFFFFF"));
        canvas.drawPath(needle, needlePaint);
        canvas.drawCircle(cx, cy, 12f, hubPaint);
    }
}
