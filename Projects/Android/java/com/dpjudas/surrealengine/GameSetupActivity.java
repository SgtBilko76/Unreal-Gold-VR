package com.dpjudas.surrealengine;

import java.io.File;
import java.util.Locale;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.StatFs;
import android.util.Log;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

// First-run setup for APKs without game data: offers to download the free original game disc
// from OldUnreal (see GameIsoInstaller) and install it into the game directory, then starts the
// game. This is a regular (non-VR) activity, so the headset shows it as a normal 2D panel that
// can be used with the controller pointer - the immersive SurrealEngineActivity can't show
// Android views. SurrealEngineActivity opens it when the game files are missing.
public class GameSetupActivity extends Activity
{
	private static final String TAG = "SurrealEngine";

	private static volatile boolean sRunning;

	private TextView mText;
	private LinearLayout mButtons;
	private String dir;
	private GameIsoInstaller iso;

	static boolean downloadAvailable(Context context)
	{
		return createInstaller(context).isConfigured();
	}

	private static GameIsoInstaller createInstaller(Context context)
	{
		return new GameIsoInstaller(context.getString(R.string.game_iso_layout), context.getString(R.string.game_iso_urls), context.getString(R.string.game_iso_size),
				context.getString(R.string.game_iso_sha256), context.getString(R.string.game_iso_name),
				new File(context.getString(R.string.game_data_dir)));
	}

	@Override protected void onCreate(Bundle icicle)
	{
		super.onCreate(icicle);
		dir = getString(R.string.game_data_dir);
		iso = createInstaller(this);

		LinearLayout panel = new LinearLayout(this);
		panel.setOrientation(LinearLayout.VERTICAL);
		panel.setGravity(Gravity.CENTER_HORIZONTAL);
		panel.setBackgroundColor(Color.rgb(16, 16, 20));
		panel.setPadding(64, 48, 64, 48);

		TextView title = new TextView(this);
		title.setText(getString(R.string.app_name));
		title.setTextColor(Color.WHITE);
		title.setTextSize(30.0f);
		title.setGravity(Gravity.CENTER);
		panel.addView(title);

		mText = new TextView(this);
		mText.setTextColor(Color.rgb(220, 220, 220));
		mText.setTextSize(20.0f);
		mText.setGravity(Gravity.CENTER);
		mText.setPadding(0, 32, 0, 32);
		panel.addView(mText);

		mButtons = new LinearLayout(this);
		mButtons.setOrientation(LinearLayout.HORIZONTAL);
		mButtons.setGravity(Gravity.CENTER);
		panel.addView(mButtons);

		ScrollView scroll = new ScrollView(this);
		scroll.setFillViewport(true);
		scroll.setBackgroundColor(Color.rgb(16, 16, 20));
		scroll.addView(panel);
		setContentView(scroll);

		if (GameIsoInstaller.gameDataPresent(new File(dir)))
			startGame();
		else if (sRunning)
			showProgress("Installing...");
		else
			showOffer(null);
	}

	private void setButtons(Button... buttons)
	{
		mButtons.removeAllViews();
		for (Button b : buttons)
		{
			LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
			lp.setMargins(24, 0, 24, 0);
			mButtons.addView(b, lp);
		}
	}

	private Button button(String label, Runnable onClick)
	{
		Button b = new Button(this);
		b.setText(label);
		b.setTextSize(20.0f);
		b.setAllCaps(false);
		b.setPadding(48, 24, 48, 24);
		b.setOnClickListener(v -> onClick.run());
		return b;
	}

	private void showOffer(String error)
	{
		String game = getString(R.string.game_name);
		StringBuilder text = new StringBuilder();
		if (error != null)
			text.append(error).append("\n\n");
		else
			text.append("The ").append(game).append(" game files are not installed yet.\n\n");
		text.append(game).append(" is free: Epic Games allows OldUnreal (oldunreal.com) to share the original game disc. ")
				.append("This app can download it (").append(iso.downloadSize() >> 20).append(" MB) and install it for you.\n\n")
				.append("The Epic Games Terms of Service apply to this game:\nlegal.epicgames.com/en-US/epicgames/tos\n\n")
				.append("Stay connected to Wi-Fi and keep the headset on until it finishes.\n")
				.append("(You can also copy your own game folders to ").append(dir).append(" instead.)");
		mText.setText(text.toString());
		setButtons(button(error != null ? "Try again" : "I agree - download and install", this::startInstall),
				button("Quit", this::finish));
	}

	private void showProgress(String text)
	{
		mText.setText(text);
		setButtons();
	}

	private void startInstall()
	{
		if (sRunning)
			return;

		new File(dir).mkdirs();
		long free = new StatFs(dir).getAvailableBytes();
		if (free < iso.requiredFreeSpace())
		{
			showOffer(String.format(Locale.ROOT, "Not enough free space on the headset: %d MB are needed, %d MB are free.",
					iso.requiredFreeSpace() >> 20, free >> 20));
			return;
		}

		sRunning = true;
		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
		showProgress("Connecting...");

		new Thread(() ->
		{
			String error;
			try
			{
				error = iso.run(text -> runOnUiThread(() -> showProgress(text)));
			}
			catch (RuntimeException e)
			{
				Log.e(TAG, "Game install failed", e);
				error = "The installation failed unexpectedly.";
			}
			if (error == null && !GameIsoInstaller.gameDataPresent(new File(dir)))
				error = "The game disc did not contain the expected files.";
			sRunning = false;

			final String result = error;
			runOnUiThread(() ->
			{
				if (isFinishing() || isDestroyed())
					return;
				getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
				if (result == null)
					startGame();
				else
					showOffer(result);
			});
		}, "GameIsoInstaller").start();
	}

	private void startGame()
	{
		Intent game = new Intent(this, SurrealEngineActivity.class);
		game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
		startActivity(game);
		finish();
	}
}
