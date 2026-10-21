package android.view;
import android.content.Context;
import android.graphics.Canvas;
public class View {
    public static final int VISIBLE = 0;
    public static final int INVISIBLE = 4;
    public static final int GONE = 8;
    public interface OnClickListener { void onClick(View v); }
    public interface OnTouchListener { boolean onTouch(View v, MotionEvent event); }
    public static class MeasureSpec {
        public static final int UNSPECIFIED = 0;
        public static final int EXACTLY = 1 << 30;
        public static final int AT_MOST = 2 << 30;
        public static int makeMeasureSpec(int size, int mode) { throw new RuntimeException("Stub!"); }
        public static int getMode(int measureSpec) { throw new RuntimeException("Stub!"); }
        public static int getSize(int measureSpec) { throw new RuntimeException("Stub!"); }
    }
    public View(Context context) { throw new RuntimeException("Stub!"); }
    public Context getContext() { throw new RuntimeException("Stub!"); }
    public android.content.res.Resources getResources() { throw new RuntimeException("Stub!"); }
    public void setBackgroundColor(int color) { throw new RuntimeException("Stub!"); }
    public void setPadding(int left, int top, int right, int bottom) { throw new RuntimeException("Stub!"); }
    public void setOnClickListener(OnClickListener l) { throw new RuntimeException("Stub!"); }
    public void setOnTouchListener(OnTouchListener l) { throw new RuntimeException("Stub!"); }
    public void setClickable(boolean clickable) { throw new RuntimeException("Stub!"); }
    public void setEnabled(boolean enabled) { throw new RuntimeException("Stub!"); }
    public boolean performClick() { throw new RuntimeException("Stub!"); }
    public void invalidate() { throw new RuntimeException("Stub!"); }
    public void requestLayout() { throw new RuntimeException("Stub!"); }
    public final int getWidth() { throw new RuntimeException("Stub!"); }
    public final int getHeight() { throw new RuntimeException("Stub!"); }
    public final int getMeasuredWidth() { throw new RuntimeException("Stub!"); }
    public final int getMeasuredHeight() { throw new RuntimeException("Stub!"); }
    protected final void setMeasuredDimension(int w, int h) { throw new RuntimeException("Stub!"); }
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) { throw new RuntimeException("Stub!"); }
    protected void onSizeChanged(int w, int h, int oldw, int oldh) { throw new RuntimeException("Stub!"); }
    protected void onDraw(Canvas canvas) { throw new RuntimeException("Stub!"); }
    public boolean onTouchEvent(MotionEvent event) { throw new RuntimeException("Stub!"); }
    public void setId(int id) { throw new RuntimeException("Stub!"); }
    public int getId() { throw new RuntimeException("Stub!"); }
    public final View findViewById(int id) { throw new RuntimeException("Stub!"); }
    public void setVisibility(int visibility) { throw new RuntimeException("Stub!"); }
    public void setLayoutParams(ViewGroup.LayoutParams params) { throw new RuntimeException("Stub!"); }
    public ViewGroup.LayoutParams getLayoutParams() { throw new RuntimeException("Stub!"); }
    public boolean post(Runnable action) { throw new RuntimeException("Stub!"); }
    public boolean postDelayed(Runnable action, long delayMillis) { throw new RuntimeException("Stub!"); }
    public boolean removeCallbacks(Runnable action) { throw new RuntimeException("Stub!"); }
}
