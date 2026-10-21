package android.graphics;
public class Paint {
    public static final int ANTI_ALIAS_FLAG = 1;
    public enum Style { FILL, STROKE, FILL_AND_STROKE }
    public enum Align { LEFT, CENTER, RIGHT }
    public Paint() { throw new RuntimeException("Stub!"); }
    public Paint(int flags) { throw new RuntimeException("Stub!"); }
    public void setColor(int color) { throw new RuntimeException("Stub!"); }
    public int getColor() { throw new RuntimeException("Stub!"); }
    public void setAlpha(int a) { throw new RuntimeException("Stub!"); }
    public void setStrokeWidth(float width) { throw new RuntimeException("Stub!"); }
    public void setStyle(Style style) { throw new RuntimeException("Stub!"); }
    public void setTextSize(float textSize) { throw new RuntimeException("Stub!"); }
    public void setTextAlign(Align align) { throw new RuntimeException("Stub!"); }
    public void setAntiAlias(boolean aa) { throw new RuntimeException("Stub!"); }
    public float measureText(String text) { throw new RuntimeException("Stub!"); }
}
