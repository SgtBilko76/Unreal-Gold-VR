package com.dpjudas.surrealengine;

import static android.system.Os.setenv;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

// Bare Activity hosting a SurfaceView, mirroring the lifecycle-forwarding pattern used by
// Team Beef Studios' QuakeQuest (GLES3JNIActivity). SurrealEngine renders via Vulkan/OpenXR,
// so there is no GLES/EGL library to preload here - only the OpenXR loader and the engine's
// own native library.
//
// Beta packaging: the APK carries the game data and SurrealEngine.pk3 as assets
// (Projects/Android/assets-game, see build.gradle). On first launch (or after an update that
// bumps versionCode) they are extracted into the game directory on a background thread while
// a progress panel is shown, and only then is the native engine created. The native side is
// unchanged: it still just reads the game directory, so a sideloaded install works exactly
// as before, and files the user places there (commandline.txt, edited .ini files) are left
// alone by the extraction.
@SuppressLint("SdCardPath") public class SurrealEngineActivity extends Activity implements SurfaceHolder.Callback
{
	private static String manufacturer = "";

	static
	{
		manufacturer = Build.MANUFACTURER.toLowerCase(Locale.ROOT);
		if (manufacturer.contains("oculus"))
		{
			manufacturer = "meta";
		}

		try
		{
			System.loadLibrary("openxr_loader");
		} catch (Throwable e)
		{}

		try
		{
			setenv("OPENXR_HMD", manufacturer, true);
		} catch (Exception e)
		{}

		System.loadLibrary("surrealengine");
	}

	private static final String TAG = "SurrealEngine";

	// Asset layout (see build.gradle's assets.srcDirs): "game/<System|Maps|...>/..." plus the
	// engine's own resource archive at the root. The marker file records which APK version
	// last extracted, so an updated APK re-extracts (overwriting the bundled files only).
	private static final String BUNDLED_GAME_ASSET_DIR = "game";
	private static final String ENGINE_PK3 = "SurrealEngine.pk3";
	private static final String INSTALL_MARKER = ".bundled-game-version";

	private SurfaceHolder mSurfaceHolder;
	private long mNativeHandle;
	private boolean mStarted;
	private boolean mResumed;
	private TextView mStatusText;

	String dir;

	private static final int REQUEST_MANAGE_ALL_FILES = 3301;
	private static final int REQUEST_WRITE_EXTERNAL_STORAGE = 3302;
	private static final int REQUEST_READ_EXTERNAL_STORAGE = 3303;

	@Override protected void onCreate(Bundle icicle)
	{
		Log.v(TAG, "SurrealEngineActivity::onCreate()");
		super.onCreate(icicle);

		FrameLayout root = new FrameLayout(this);
		SurfaceView view = new SurfaceView(this);
		root.addView(view, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

		// Shown while the bundled game data is being extracted (the activity is still a flat
		// 2D panel at that point, before the OpenXR session starts).
		mStatusText = new TextView(this);
		mStatusText.setTextColor(Color.WHITE);
		mStatusText.setBackgroundColor(Color.BLACK);
		mStatusText.setTextSize(28.0f);
		mStatusText.setGravity(Gravity.CENTER);
		mStatusText.setVisibility(View.GONE);
		root.addView(mStatusText, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

		setContentView(root);
		view.getHolder().addCallback(this);

		// Game directory. Sideloaded content still goes here too; the bundled data is
		// extracted into it on first launch (see installBundledGameThenCreate).
		dir = getString(R.string.game_data_dir); // per-flavor (build.gradle): Gold /sdcard/SurrealEngine, UT99 /sdcard/SurrealEngineUT

		checkPermissionsAndInitialize();
	}

	private void checkPermissionsAndInitialize()
	{
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager())
		{
			Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
			Uri uri = Uri.fromParts("package", getPackageName(), null);
			intent.setData(uri);
			startActivityForResult(intent, REQUEST_MANAGE_ALL_FILES);
			finishAffinity();
		}
		else if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R &&
				ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE)
						!= PackageManager.PERMISSION_GRANTED)
		{
			ActivityCompat.requestPermissions(this,
					new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_WRITE_EXTERNAL_STORAGE);
		}
		else if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R &&
				ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
						!= PackageManager.PERMISSION_GRANTED)
		{
			ActivityCompat.requestPermissions(this,
					new String[]{Manifest.permission.READ_EXTERNAL_STORAGE}, REQUEST_READ_EXTERNAL_STORAGE);
		}
		else
		{
			installBundledGameThenCreate();
		}
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data)
	{
		super.onActivityResult(requestCode, resultCode, data);
		finishAffinity();
		System.exit(0);
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults)
	{
		super.onRequestPermissionsResult(requestCode, permissions, grantResults);
		if ((requestCode == REQUEST_READ_EXTERNAL_STORAGE || requestCode == REQUEST_WRITE_EXTERNAL_STORAGE)
				&& grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED)
		{
			checkPermissionsAndInitialize();
		}
		else
		{
			System.exit(0);
		}
	}

	private String currentVersionTag()
	{
		try
		{
			return String.valueOf(getPackageManager().getPackageInfo(getPackageName(), 0).versionCode);
		}
		catch (PackageManager.NameNotFoundException e)
		{
			return "unknown";
		}
	}

	private boolean bundledGameInstalled()
	{
		File marker = new File(dir, INSTALL_MARKER);
		if (!marker.exists())
			return false;
		try
		{
			String installed = new String(Files.readAllBytes(marker.toPath()), StandardCharsets.UTF_8).trim();
			return installed.equals(currentVersionTag());
		}
		catch (IOException e)
		{
			return false;
		}
	}

	private boolean hasBundledGame()
	{
		try
		{
			String[] entries = getAssets().list(BUNDLED_GAME_ASSET_DIR);
			return entries != null && entries.length > 0;
		}
		catch (IOException e)
		{
			return false;
		}
	}

	private void installBundledGameThenCreate()
	{
		new File(dir).mkdirs();

		if (!hasBundledGame())
		{
			// APK without game data (the public release): the engine's own pk3 still ships in
			// it, and the game itself is either sideloaded or downloaded from OldUnreal by
			// GameSetupActivity - a regular 2D panel, since nothing this immersive activity
			// draws with Android views is visible on the headset before the OpenXR session.
			installEnginePk3();
			if (!GameIsoInstaller.gameDataPresent(new File(dir)) && GameSetupActivity.downloadAvailable(this))
			{
				Intent setup = new Intent(this, GameSetupActivity.class);
				setup.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
				startActivity(setup);
				finish();
				return;
			}
			create();
			return;
		}

		if (bundledGameInstalled())
		{
			create();
			return;
		}

		mStatusText.setText("Installing " + getString(R.string.app_name) + " game data...\nThis only happens once.");
		mStatusText.setVisibility(View.VISIBLE);

		Thread installer = new Thread(() ->
		{
			boolean ok = extractBundledGame();
			runOnUiThread(() ->
			{
				if (ok)
				{
					mStatusText.setVisibility(View.GONE);
					create();
				}
				else
				{
					mStatusText.setText("Installing the game data failed.\nCheck free space on the headset and restart the app.");
				}
			});
		}, "GameDataInstaller");
		installer.start();
	}

	// Copies the engine's resource archive out of the APK when it is missing or outdated.
	private void installEnginePk3()
	{
		File target = new File(dir, ENGINE_PK3);
		long assetLength = -1;
		try (android.content.res.AssetFileDescriptor fd = getAssets().openFd(ENGINE_PK3))
		{
			assetLength = fd.getLength();
		}
		catch (IOException e)
		{
			// stored compressed, or not in this APK
		}
		if (target.exists() && target.length() == assetLength)
			return;
		try
		{
			getAssets().open(ENGINE_PK3).close();
		}
		catch (IOException e)
		{
			return; // APK built without the pk3: keep whatever the user sideloaded
		}
		copyAssetFile(ENGINE_PK3, target);
	}

	// Copies assets/game/** into dir/** and the engine pk3 into dir, then writes the marker.
	private boolean extractBundledGame()
	{
		AssetManager assets = getAssets();
		List<String> files = new ArrayList<>();
		try
		{
			collectAssetFiles(assets, BUNDLED_GAME_ASSET_DIR, files);
		}
		catch (IOException e)
		{
			Log.e(TAG, "Listing bundled game assets failed", e);
			return false;
		}

		int done = 0;
		int total = files.size() + 1;
		for (String asset : files)
		{
			String relative = asset.substring(BUNDLED_GAME_ASSET_DIR.length() + 1);
			if (!copyAssetFile(asset, new File(dir, relative)))
				return false;
			done++;
			if ((done % 25) == 0 || done == files.size())
				reportProgress(done, total);
		}
		if (!copyAssetFile(ENGINE_PK3, new File(dir, ENGINE_PK3)))
			return false;
		reportProgress(total, total);

		try
		{
			Files.write(new File(dir, INSTALL_MARKER).toPath(), currentVersionTag().getBytes(StandardCharsets.UTF_8));
		}
		catch (IOException e)
		{
			Log.e(TAG, "Writing install marker failed", e);
			return false;
		}
		Log.v(TAG, "Bundled game data installed to " + dir);
		return true;
	}

	private void reportProgress(int done, int total)
	{
		final int percent = (int)(100L * done / Math.max(total, 1));
		runOnUiThread(() -> mStatusText.setText("Installing " + getString(R.string.app_name) + " game data...\n" + percent + "%\nThis only happens once."));
	}

	private static void collectAssetFiles(AssetManager assets, String path, List<String> out) throws IOException
	{
		String[] entries = assets.list(path);
		if (entries == null || entries.length == 0)
		{
			out.add(path); // a file (directories list their children)
			return;
		}
		for (String entry : entries)
			collectAssetFiles(assets, path + "/" + entry, out);
	}

	private boolean copyAssetFile(String assetName, File target)
	{
		File parent = target.getParentFile();
		if (parent != null)
			parent.mkdirs();
		try (InputStream in = getAssets().open(assetName);
			 OutputStream out = new FileOutputStream(target))
		{
			byte[] buf = new byte[256 * 1024];
			int count;
			while ((count = in.read(buf)) > 0)
				out.write(buf, 0, count);
			return true;
		}
		catch (IOException e)
		{
			Log.e(TAG, "Copying bundled asset " + assetName + " failed", e);
			return false;
		}
	}

	public void create()
	{
		new File(dir).mkdirs();

		try
		{
			setenv("SURREALENGINE_DIR", dir, true);
			// Directory::localAppData() (SurrealEngine/Utils/File.cpp) reads $HOME on
			// non-Windows platforms to place Settings.json/User.ini - point it at this app's
			// private storage rather than leaving it unset.
			setenv("HOME", getFilesDir().getAbsolutePath(), true);
		}
		catch (Exception ignored)
		{
			System.exit(-9);
		}

		mNativeHandle = SurrealEngineJNILib.onCreate(this, dir);

		// The engine may be created late (after the extraction above), by which time the
		// activity has already been started/resumed and the surface created - replay those
		// lifecycle events to the native side in the order it expects.
		if (mStarted)
			SurrealEngineJNILib.onStart(mNativeHandle, this);
		if (mResumed)
			SurrealEngineJNILib.onResume(mNativeHandle);
		if (mSurfaceHolder != null)
			SurrealEngineJNILib.onSurfaceCreated(mNativeHandle, mSurfaceHolder.getSurface());
	}

	public void shutdown()
	{
		System.exit(0);
	}

	@Override protected void onStart()
	{
		Log.v(TAG, "SurrealEngineActivity::onStart()");
		super.onStart();
		mStarted = true;
		if (mNativeHandle != 0)
			SurrealEngineJNILib.onStart(mNativeHandle, this);
	}

	@Override protected void onResume()
	{
		Log.v(TAG, "SurrealEngineActivity::onResume()");
		super.onResume();
		mResumed = true;
		if (mNativeHandle != 0)
			SurrealEngineJNILib.onResume(mNativeHandle);
	}

	@Override protected void onPause()
	{
		Log.v(TAG, "SurrealEngineActivity::onPause()");
		mResumed = false;
		if (mNativeHandle != 0)
			SurrealEngineJNILib.onPause(mNativeHandle);
		super.onPause();
	}

	@Override protected void onStop()
	{
		Log.v(TAG, "SurrealEngineActivity::onStop()");
		mStarted = false;
		if (mNativeHandle != 0)
			SurrealEngineJNILib.onStop(mNativeHandle);
		super.onStop();
	}

	@Override protected void onDestroy()
	{
		Log.v(TAG, "SurrealEngineActivity::onDestroy()");

		if (mSurfaceHolder != null && mNativeHandle != 0)
			SurrealEngineJNILib.onSurfaceDestroyed(mNativeHandle);

		if (mNativeHandle != 0)
			SurrealEngineJNILib.onDestroy(mNativeHandle);

		super.onDestroy();
		mNativeHandle = 0;
	}

	@Override public void surfaceCreated(SurfaceHolder holder)
	{
		Log.v(TAG, "SurrealEngineActivity::surfaceCreated()");
		mSurfaceHolder = holder;
		if (mNativeHandle != 0)
			SurrealEngineJNILib.onSurfaceCreated(mNativeHandle, holder.getSurface());
	}

	@Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height)
	{
		Log.v(TAG, "SurrealEngineActivity::surfaceChanged()");
		mSurfaceHolder = holder;
		if (mNativeHandle != 0)
			SurrealEngineJNILib.onSurfaceChanged(mNativeHandle, holder.getSurface());
	}

	@Override public void surfaceDestroyed(SurfaceHolder holder)
	{
		Log.v(TAG, "SurrealEngineActivity::surfaceDestroyed()");
		if (mNativeHandle != 0)
			SurrealEngineJNILib.onSurfaceDestroyed(mNativeHandle);
		mSurfaceHolder = null;
	}
}
