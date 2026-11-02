package uk.xa0.ward;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

public final class MainActivity extends Activity {
  private static final String TAG = "ward";
  private static final String TARGET = "tcp://127.0.0.1:45872";

  @Override protected void onCreate(Bundle state) {
    super.onCreate(state);
    TextView view = new TextView(this);
    view.setTextSize(18f);
    setContentView(view);
    String report;
    try (WardClient client = new WardClient(TARGET)) {
      String session = client.createSession("/tmp");
      report = "session " + session;
    } catch (Throwable failure) {
      report = "failed: " + failure;
    }
    Log.i(TAG, report);
    view.setText(report);
  }
}
