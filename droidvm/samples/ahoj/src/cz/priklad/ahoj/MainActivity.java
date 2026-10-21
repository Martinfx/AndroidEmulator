package cz.priklad.ahoj;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

// The first screen: a counter, a text field and a button that opens a drawing screen.
public class MainActivity extends Activity {
    private int clicks = 0;
    private TextView counter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle("Ahoj z droidvm");
        Log.d("Ahoj", "onCreate");

        float dp = getResources().getDisplayMetrics().density;
        int pad = (int) (16 * dp);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("Vítej v emulátoru napsaném od nuly!");
        title.setTextSize(22);
        title.setTextColor(Color.rgb(63, 81, 181));
        root.addView(title);

        TextView info = new TextView(this);
        info.setText("Tato aplikace běží v interpretu Dalvik bajtkódu a kreslí ji vlastní softwarový rasterizér.");
        info.setPadding(0, pad / 2, 0, pad);
        root.addView(info);

        counter = new TextView(this);
        counter.setTextSize(48);
        counter.setGravity(Gravity.CENTER_HORIZONTAL);
        updateCounter();
        root.addView(counter);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        Button minus = new Button(this);
        minus.setText("-1");
        Button plus = new Button(this);
        plus.setText("+1");
        buttons.addView(minus, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        buttons.addView(plus, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        root.addView(buttons);

        minus.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                clicks--;
                updateCounter();
            }
        });
        plus.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                clicks++;
                updateCounter();
                Log.i("Ahoj", "kliknutí: " + clicks);
                if (clicks == 10) Toast.makeText(MainActivity.this, "Deset kliknutí!", Toast.LENGTH_SHORT).show();
            }
        });

        final EditText name = new EditText(this);
        name.setHint("Napiš své jméno");
        root.addView(name);

        final TextView greeting = new TextView(this);
        greeting.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(greeting);

        Button greet = new Button(this);
        greet.setText("Pozdravit");
        greet.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String who = name.getText().toString().trim();
                String text = who.isEmpty() ? "Nejdřív napiš jméno." : "Ahoj, " + who + "!";
                greeting.setText(text);
                Log.i("Ahoj", "pozdrav: " + text);
            }
        });
        root.addView(greet);

        Button draw = new Button(this);
        draw.setText("Kreslení a animace");
        draw.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent intent = new Intent(MainActivity.this, DrawActivity.class);
                intent.putExtra("clicks", clicks);
                startActivity(intent);
            }
        });
        root.addView(draw);

        setContentView(root);
    }

    private void updateCounter() {
        counter.setText(String.valueOf(clicks));
        counter.setTextColor(clicks < 0 ? Color.RED : Color.BLACK);
    }
}
