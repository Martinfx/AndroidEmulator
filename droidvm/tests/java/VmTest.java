// A console program exercising the interpreter. tests/run_vm_test.sh runs it
// both on the host JVM and on droidvm and compares the output line by line.
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Random;

public class VmTest {
    interface Shape { double area(); default String kind() { return "shape"; } }

    static abstract class Base implements Shape {
        protected final String name;
        Base(String name) { this.name = name; }
        public String toString() { return kind() + " " + name + " area=" + String.format("%.2f", area()); }
    }

    static class Circle extends Base {
        final double r;
        Circle(double r) { super("circle"); this.r = r; }
        public double area() { return Math.PI * r * r; }
        public String kind() { return "round"; }
    }

    static class Rect extends Base {
        final int w, h;
        Rect(int w, int h) { super("rect"); this.w = w; this.h = h; }
        public double area() { return w * h; }
    }

    enum Color { RED, GREEN, BLUE }

    static int counter;
    static final String GREETING;
    static { GREETING = "static init ok"; counter = 40; }

    static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

    static String classify(int v) {
        switch (v) {
            case 1: return "one";
            case 2: return "two";
            case 3: return "three";
            case 100: return "hundred";
            case -7: return "minus seven";
            default: return "other";
        }
    }

    public static void main(String[] args) {
        System.out.println("Hello from Dalvik!");
        System.out.println(GREETING + " " + (counter + 2));

        // integer arithmetic and overflow
        int a = Integer.MAX_VALUE;
        System.out.println(a + 1);
        System.out.println(-17 / 5 + " " + -17 % 5 + " " + (-17 >> 2) + " " + (-17 >>> 28));
        long big = 1L << 40;
        System.out.println(big * 3 + 7);
        System.out.println(Long.MAX_VALUE / 3);
        System.out.println(fib(20));

        // floating point and conversions
        float f = 1.1f;
        double d = 2.5;
        System.out.println(f * 3);
        System.out.println(d / 3);
        System.out.println((int) 3.99 + " " + (int) -3.99 + " " + (long) 1e20 + " " + (int) Float.NaN);
        System.out.println(0.1 + 0.2);
        System.out.println(100.0f + " " + 1e10 + " " + 0.0001);
        System.out.println((char) ('a' + 2) + " " + (byte) 300 + " " + (short) 70000);

        // strings
        String s = "Ahoj, světe";
        System.out.println(s.length() + " " + s.toUpperCase() + " " + s.substring(6) + " " + s.indexOf("svě"));
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 5; i++) sb.append(i).append(',');
        sb.setLength(sb.length() - 1);
        System.out.println(sb.reverse().toString());
        System.out.println("a,b,,c".split(",").length + " " + "x".equals("x") + " " + "abc".compareTo("abd"));
        System.out.println(String.format("%d|%5s|%-4d|%x|%.3f", 42, "hi", 7, 255, 3.14159));
        System.out.println("hash " + "hello".hashCode());

        // arrays
        int[] nums = {5, 3, 9, 1, 7};
        int sum = 0;
        for (int n : nums) sum += n;
        int[][] grid = new int[3][4];
        grid[2][3] = 12;
        System.out.println(sum + " " + nums.length + " " + grid[2][3] + " " + grid[1].length);
        char[] cs = "dalvik".toCharArray();
        cs[0] = 'D';
        System.out.println(new String(cs));
        long[] longs = new long[3];
        longs[1] = 1234567890123L;
        System.out.println(longs[1] + longs[0]);

        // objects, interfaces, default methods
        List<Shape> shapes = new ArrayList<>();
        shapes.add(new Circle(1.5));
        shapes.add(new Rect(3, 4));
        for (Shape sh : shapes) System.out.println(sh);
        System.out.println(shapes.get(1) instanceof Base);
        System.out.println(shapes.get(0) instanceof Rect);

        // switch statements
        for (int v : new int[] {1, 3, 100, -7, 55}) System.out.print(classify(v) + " ");
        System.out.println();
        String word = "beta";
        switch (word) {
            case "alpha": System.out.println("A"); break;
            case "beta": System.out.println("B"); break;
            default: System.out.println("?");
        }

        // exceptions
        try {
            int[] small = new int[2];
            small[5] = 1;
        } catch (ArrayIndexOutOfBoundsException e) {
            System.out.println("caught AIOOBE");
        }
        try {
            Object o = "text";
            Integer bad = (Integer) o;
            System.out.println(bad);
        } catch (ClassCastException e) {
            System.out.println("caught CCE");
        }
        try {
            System.out.println(10 / (counter - 40));
        } catch (ArithmeticException e) {
            System.out.println("caught " + e.getMessage());
        } finally {
            System.out.println("finally runs");
        }
        try {
            throw new IllegalStateException("custom");
        } catch (RuntimeException e) {
            System.out.println("caught " + e.getMessage());
        }
        try {
            Integer.parseInt("12x");
        } catch (NumberFormatException e) {
            System.out.println("NFE " + e.getMessage());
        }

        // collections and boxing
        HashMap<String, Integer> counts = new HashMap<>();
        for (String w : "a b a c b a".split(" ")) {
            Integer c = counts.get(w);
            counts.put(w, c == null ? 1 : c + 1);
        }
        System.out.println(counts.get("a") + " " + counts.get("b") + " " + counts.get("c") + " " + counts.size());
        Integer boxed = 1000;
        System.out.println(boxed.equals(1000) + " " + (boxed.intValue() * 2));

        // enums
        Color c = Color.valueOf("GREEN");
        System.out.println(c + " " + c.ordinal() + " " + Color.values().length);
        switch (c) {
            case RED: System.out.println("is red"); break;
            case GREEN: System.out.println("is green"); break;
            default: System.out.println("is blue");
        }

        // anonymous classes and captured variables
        final int base = 10;
        Runnable r = new Runnable() {
            public void run() { System.out.println("runnable " + (base + counter)); }
        };
        r.run();

        // java.util.Random is deterministic for a seed
        Random rnd = new Random(42);
        System.out.println(rnd.nextInt(100) + " " + rnd.nextInt(100) + " " + rnd.nextInt());

        // Math
        System.out.println(Math.max(3, 9) + " " + Math.abs(-4.5) + " " + Math.round(2.5f) + " " + Math.sqrt(144.0));
        System.out.println("done");
    }
}
