package android.view;
import android.content.Context;
public abstract class ViewGroup extends View implements ViewParent {
    public static class LayoutParams {
        public static final int MATCH_PARENT = -1;
        public static final int WRAP_CONTENT = -2;
        public int width;
        public int height;
        public LayoutParams(int width, int height) { throw new RuntimeException("Stub!"); }
    }
    public static class MarginLayoutParams extends LayoutParams {
        public int leftMargin, topMargin, rightMargin, bottomMargin;
        public MarginLayoutParams(int width, int height) { super(width, height); }
        public void setMargins(int left, int top, int right, int bottom) { throw new RuntimeException("Stub!"); }
    }
    public ViewGroup(Context context) { super(context); }
    public void addView(View child) { throw new RuntimeException("Stub!"); }
    public void addView(View child, int index) { throw new RuntimeException("Stub!"); }
    public void addView(View child, LayoutParams params) { throw new RuntimeException("Stub!"); }
    public void removeView(View view) { throw new RuntimeException("Stub!"); }
    public void removeAllViews() { throw new RuntimeException("Stub!"); }
    public int getChildCount() { throw new RuntimeException("Stub!"); }
    public View getChildAt(int index) { throw new RuntimeException("Stub!"); }
}
