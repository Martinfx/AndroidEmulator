package cz.priklad.ahoj;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;

// A bouncing ball animated with Handler.postDelayed, plus lines drawn with a finger.
public class BallView extends View {
    private final Paint ballPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint framePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ArrayList<float[]> lines = new ArrayList<>();
    private final Handler handler = new Handler();
    private float x = 100, y = 100, vx = 9, vy = 13, radius = 40;
    private float lastX, lastY;
    private int frames = 0;

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            step();
            invalidate();
            handler.postDelayed(this, 16);
        }
    };

    public BallView(Context context) {
        super(context);
        setBackgroundColor(Color.rgb(250, 250, 240));
        ballPaint.setColor(Color.rgb(255, 64, 129));
        linePaint.setColor(Color.rgb(48, 63, 159));
        linePaint.setStrokeWidth(8);
        textPaint.setColor(Color.DKGRAY);
        textPaint.setTextSize(32);
        framePaint.setColor(Color.LTGRAY);
        framePaint.setStyle(Paint.Style.STROKE);
        framePaint.setStrokeWidth(4);
        handler.post(tick);
    }

    private void step() {
        frames++;
        x += vx;
        y += vy;
        if (x < radius || x > getWidth() - radius) { vx = -vx; x = Math.max(radius, Math.min(x, getWidth() - radius)); }
        if (y < radius || y > getHeight() - radius) { vy = -vy; y = Math.max(radius, Math.min(y, getHeight() - radius)); }
    }

    public void clear() {
        lines.clear();
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.drawRoundRect(new RectF(8, 8, getWidth() - 8, getHeight() - 8), 24, 24, framePaint);
        for (float[] l : lines) canvas.drawLine(l[0], l[1], l[2], l[3], linePaint);
        canvas.drawCircle(x, y, radius, ballPaint);
        canvas.drawText("Táhni prstem (myší) a kresli. Snímek " + frames, 32, getHeight() - 32, textPaint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        float ex = event.getX(), ey = event.getY();
        if (event.getAction() == MotionEvent.ACTION_DOWN) {
            lastX = ex;
            lastY = ey;
        } else if (event.getAction() == MotionEvent.ACTION_MOVE) {
            lines.add(new float[] {lastX, lastY, ex, ey});
            lastX = ex;
            lastY = ey;
            invalidate();
        }
        return true;
    }
}
