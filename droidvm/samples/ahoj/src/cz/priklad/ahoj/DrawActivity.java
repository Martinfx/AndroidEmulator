package cz.priklad.ahoj;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.view.View;

// The second screen: a custom View with an animation and finger painting.
public class DrawActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int clicks = getIntent().getIntExtra("clicks", 0);
        setTitle("Kreslení (kliknutí: " + clicks + ")");
        android.util.Log.i("Ahoj", "DrawActivity, kliknutí z intentu: " + clicks);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        final BallView ball = new BallView(this);
        root.addView(ball, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        LinearLayout bar = new LinearLayout(this);
        Button clear = new Button(this);
        clear.setText("Smazat");
        clear.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) { ball.clear(); }
        });
        Button back = new Button(this);
        back.setText("Zpět");
        back.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                android.util.Log.i("Ahoj", "zpět");
                finish();
            }
        });
        bar.addView(clear, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        bar.addView(back, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        root.addView(bar);

        setContentView(root);
    }
}
