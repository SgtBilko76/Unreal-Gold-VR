package com.dpjudas.surrealengine;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import android.util.Log;

// Installs the game from its original disc image when the APK carries no game data.
//
// Epic Games approved OldUnreal distributing the original Unreal Gold and Unreal Tournament GOTY
// disc images for free (oldunreal.com "Full Game Installers"). This class does what OldUnreal's
// own installers do (https://github.com/OldUnreal/FullGameInstallers): download the ISO from
// OldUnreal's file servers (archive.org as fallback), check its size and SHA-256, and copy the
// disc contents into the game directory the way a normal install lays them out ("layout": which
// disc it is). The discs already carry versions Surreal Engine supports (Unreal Gold 226b, UT
// 436), so - unlike OldUnreal's installer - no patch is applied. The UT disc stores its maps
// compressed (.unr.uz); those are unpacked with UzDecoder.
//
// The download resumes after an interruption (the partial file is kept next to the game
// directory), and a marker file makes an interrupted extraction start over on the next launch.
final class GameIsoInstaller
{
	private static final String TAG = "SurrealEngine";

	static final String INCOMPLETE_MARKER = ".iso-install-incomplete";
	private static final String WORK_DIR = "Installer";

	interface Progress
	{
		void status(String text);
	}

	static final String LAYOUT_UNREAL_GOLD = "unrealgold";
	static final String LAYOUT_UT_GOTY = "utgoty";

	private final String layout;
	private final String[] urls;
	private final long expectedSize;
	private final String expectedSha256;
	private final File gameDir;
	private final File workDir;
	private final File isoFile;
	private final File partFile;

	GameIsoInstaller(String layout, String urlList, String size, String sha256, String isoName, File gameDir)
	{
		this.layout = layout.trim();
		this.urls = urlList.trim().isEmpty() ? new String[0] : urlList.trim().split("\\|");
		this.expectedSize = size.trim().isEmpty() ? -1 : Long.parseLong(size.trim());
		this.expectedSha256 = sha256.trim().toLowerCase(Locale.ROOT);
		this.gameDir = gameDir;
		this.workDir = new File(gameDir, WORK_DIR);
		this.isoFile = new File(workDir, isoName);
		this.partFile = new File(workDir, isoName + ".part");
	}

	// A usable install: the core packages are there and no disc-image extraction was cut short.
	static boolean gameDataPresent(File gameDir)
	{
		return new File(gameDir, "System/Core.u").exists() && new File(gameDir, "System/Engine.u").exists()
				&& !new File(gameDir, INCOMPLETE_MARKER).exists();
	}

	boolean isConfigured()
	{
		return urls.length > 0 && expectedSize > 0 && !expectedSha256.isEmpty()
				&& (layout.equals(LAYOUT_UNREAL_GOLD) || layout.equals(LAYOUT_UT_GOTY));
	}

	long downloadSize()
	{
		return expectedSize;
	}

	// Bytes still needed on the storage: the rest of the download plus the extracted files
	// (about the size of the disc image; UT's unpacked maps add ~100 MB).
	long requiredFreeSpace()
	{
		long have = isoFile.exists() ? isoFile.length() : (partFile.exists() ? partFile.length() : 0);
		return (expectedSize - have) + expectedSize + 192L * 1024 * 1024;
	}

	// Runs on a background thread. Returns null on success, otherwise a message for the user.
	String run(Progress progress)
	{
		workDir.mkdirs();

		if (!isoFile.exists() || isoFile.length() != expectedSize)
		{
			String error = download(progress);
			if (error != null)
				return error;
		}

		progress.status("Verifying the download...");
		String sha;
		try
		{
			sha = sha256(isoFile, progress);
		}
		catch (Exception e)
		{
			Log.e(TAG, "Hashing the disc image failed", e);
			return "Could not read the downloaded disc image.";
		}
		if (!sha.equals(expectedSha256))
		{
			Log.e(TAG, "Disc image checksum mismatch: " + sha);
			isoFile.delete();
			return "The download was corrupted and has been deleted.";
		}

		File marker = new File(gameDir, INCOMPLETE_MARKER);
		try
		{
			new FileOutputStream(marker).close();
			extract(progress);
		}
		catch (IOException e)
		{
			Log.e(TAG, "Extracting the disc image failed", e);
			return "Installing the game files failed. Check the free space on the headset.";
		}
		marker.delete();

		isoFile.delete();
		workDir.delete();
		Log.v(TAG, "Game installed from disc image into " + gameDir);
		return null;
	}

	private String download(Progress progress)
	{
		for (String url : urls)
		{
			try
			{
				if (downloadFrom(url, progress))
				{
					if (!partFile.renameTo(isoFile))
						return "Could not save the downloaded disc image.";
					return null;
				}
			}
			catch (IOException e)
			{
				Log.w(TAG, "Download from " + url + " failed", e);
			}
		}
		return "No download server could be reached. Check the headset's Wi-Fi connection.";
	}

	// Returns true once partFile holds the complete image. Resumes a partial file via HTTP Range.
	private boolean downloadFrom(String url, Progress progress) throws IOException
	{
		long have = partFile.exists() ? partFile.length() : 0;
		if (have > expectedSize)
		{
			partFile.delete();
			have = 0;
		}
		if (have == expectedSize)
			return true;

		Log.v(TAG, "Downloading " + url + " from byte " + have);
		HttpURLConnection conn = (HttpURLConnection)new URL(url).openConnection();
		conn.setConnectTimeout(15000);
		conn.setReadTimeout(30000);
		conn.setInstanceFollowRedirects(true);
		conn.setRequestProperty("User-Agent", "UnrealGoldVR-Installer");
		if (have > 0)
			conn.setRequestProperty("Range", "bytes=" + have + "-");

		try
		{
			int code = conn.getResponseCode();
			boolean append;
			if (code == HttpURLConnection.HTTP_PARTIAL && have > 0)
				append = true;
			else if (code == HttpURLConnection.HTTP_OK)
			{
				append = false; // server ignored the Range request - start over
				have = 0;
			}
			else
			{
				Log.w(TAG, "Download from " + url + ": HTTP " + code);
				return false;
			}

			byte[] buf = new byte[256 * 1024];
			long lastReport = 0;
			try (InputStream in = conn.getInputStream();
				 FileOutputStream out = new FileOutputStream(partFile, append))
			{
				int count;
				while ((count = in.read(buf)) > 0)
				{
					out.write(buf, 0, count);
					have += count;
					long now = System.currentTimeMillis();
					if (now - lastReport > 500)
					{
						lastReport = now;
						progress.status(String.format(Locale.ROOT, "Downloading the game disc from %s...\n%d of %d MB (%d%%)",
								new URL(url).getHost(), have >> 20, expectedSize >> 20, (int)(100L * have / expectedSize)));
					}
					if (have > expectedSize)
						break;
				}
			}
		}
		finally
		{
			conn.disconnect();
		}
		return have == expectedSize;
	}

	private String sha256(File file, Progress progress) throws Exception
	{
		MessageDigest digest = MessageDigest.getInstance("SHA-256");
		byte[] buf = new byte[1024 * 1024];
		long done = 0;
		try (InputStream in = new FileInputStream(file))
		{
			int count;
			while ((count = in.read(buf)) > 0)
			{
				digest.update(buf, 0, count);
				done += count;
				if ((done & ((32L << 20) - 1)) < count)
					progress.status("Verifying the download... " + (int)(100L * done / expectedSize) + "%");
			}
		}
		StringBuilder sb = new StringBuilder();
		for (byte b : digest.digest())
			sb.append(String.format(Locale.ROOT, "%02x", b));
		return sb.toString();
	}

	// ---- Disc image contents ----

	private static final class IsoFile
	{
		final String path; // "/"-separated, as on the disc (Joliet names, version suffix removed)
		final long offset;
		final long size;

		IsoFile(String path, long offset, long size)
		{
			this.path = path;
			this.offset = offset;
			this.size = size;
		}
	}

	private void extract(Progress progress) throws IOException
	{
		try (RandomAccessFile raf = new RandomAccessFile(isoFile, "r"))
		{
			FileChannel ch = raf.getChannel();
			List<IsoFile> files = listIsoFiles(ch);

			List<IsoFile> wanted = new ArrayList<>();
			List<String> targets = new ArrayList<>();
			long total = 0;
			for (IsoFile f : files)
			{
				String target = targetPath(f.path);
				if (target == null)
					continue;
				wanted.add(f);
				targets.add(target);
				total += f.size;
			}
			if (wanted.isEmpty())
				throw new IOException("No game files found in the disc image");

			long done = 0;
			for (int i = 0; i < wanted.size(); i++)
			{
				IsoFile f = wanted.get(i);
				String target = targets.get(i);
				File out = new File(gameDir, target);
				File parent = out.getParentFile();
				if (parent != null)
					parent.mkdirs();
				if (f.path.toLowerCase(Locale.ROOT).endsWith(".uz"))
				{
					if (f.size > 64L * 1024 * 1024)
						throw new IOException("Compressed file too large: " + f.path);
					ByteBuffer packed = read(ch, f.offset, (int)f.size);
					UzDecoder.Result unpacked = UzDecoder.decode(packed.array());
					try (FileOutputStream fos = new FileOutputStream(out))
					{
						fos.write(unpacked.data);
					}
				}
				else
				{
					try (FileOutputStream fos = new FileOutputStream(out))
					{
						FileChannel dst = fos.getChannel();
						long pos = 0;
						while (pos < f.size)
							pos += ch.transferTo(f.offset + pos, f.size - pos, dst);
					}
				}
				done += f.size;
				progress.status("Installing the game files... " + (int)(100L * done / Math.max(total, 1)) + "%");
			}

			if (layout.equals(LAYOUT_UT_GOTY))
			{
				// As OldUnreal's installer does: DM-Cybrosis][ doubles as a Domination map.
				File dm = new File(gameDir, "Maps/DM-Cybrosis][.unr");
				File dom = new File(gameDir, "Maps/DOM-Cybrosis][.unr");
				if (dm.exists() && !dom.exists())
					java.nio.file.Files.copy(dm.toPath(), dom.toPath());
			}
		}
	}

	// Maps a disc path to its place in the game directory (null = not installed).
	String targetPath(String isoPath)
	{
		return layout.equals(LAYOUT_UT_GOTY) ? utGotyTargetPath(isoPath) : unrealGoldTargetPath(isoPath);
	}

	// Unreal Tournament GOTY CD1: folders are already cased normally on the disc. Keeps what
	// OldUnreal's installer keeps (minus the patch it adds): the game folders, English text only,
	// no default UnrealTournament.ini/User.ini; Maps/*.unr.uz are installed unpacked as *.unr.
	// Left out: installers, DirectX, GameSpy, NetGamesUSA.com, Web, Logs and System400 (the
	// v400 network-compatibility packages).
	static String utGotyTargetPath(String isoPath)
	{
		String[] parts = isoPath.split("/");
		if (parts.length < 2)
			return null;

		String folder;
		switch (parts[0].toLowerCase(Locale.ROOT))
		{
			case "help": folder = "Help"; break;
			case "maps": folder = "Maps"; break;
			case "music": folder = "Music"; break;
			case "sounds": folder = "Sounds"; break;
			case "system": folder = "System"; break;
			case "textures": folder = "Textures"; break;
			default: return null;
		}

		String name = parts[parts.length - 1];
		String lower = name.toLowerCase(Locale.ROOT);

		if (folder.equals("System"))
		{
			if (parts.length > 2)
				return null;
			if (lower.equals("unrealtournament.ini") || lower.equals("user.ini"))
				return null;
			for (String ext : new String[] {".ctt", ".det", ".elt", ".est", ".frt", ".itt", ".nlt", ".ptt", ".rut"})
				if (lower.endsWith(ext))
					return null;
		}

		if (folder.equals("Maps") && lower.endsWith(".uz"))
			name = name.substring(0, name.length() - 3);

		StringBuilder sb = new StringBuilder(folder);
		for (int i = 1; i < parts.length - 1; i++)
			sb.append('/').append(parts[i]);
		sb.append('/').append(name);
		return sb.toString();
	}

	// Unreal Gold: mirrors what OldUnreal's installer keeps: the game folders with normal casing,
	// English sounds and text only, and no default Unreal.ini/User.ini (the engine creates its
	// own from Default.ini).
	static String unrealGoldTargetPath(String isoPath)
	{
		String[] parts = isoPath.split("/");
		if (parts.length < 2)
			return null;

		String folder;
		switch (parts[0].toUpperCase(Locale.ROOT))
		{
			case "HELP": folder = "Help"; break;
			case "MAPS": folder = "Maps"; break;
			case "MUSIC": folder = "Music"; break;
			case "SOUNDS": folder = "Sounds"; break;
			case "SYSTEM": folder = "System"; break;
			case "TEXTURES": folder = "Textures"; break;
			default: return null; // installers, DirectX, manuals, SystemLocalized
		}

		String name = parts[parts.length - 1];
		String lower = name.toLowerCase(Locale.ROOT);

		if (folder.equals("Sounds") && parts.length > 2)
		{
			// Per-language speech packages: Sounds/int/*.uax installs as Sounds/*.uax,
			// the det/est/frt/itt translations are left out.
			if (parts.length == 3 && parts[1].equalsIgnoreCase("int"))
				return "Sounds/" + name;
			return null;
		}

		if (folder.equals("System"))
		{
			if (lower.endsWith(".det") || lower.endsWith(".est") || lower.endsWith(".frt") || lower.endsWith(".itt"))
				return null;
			if (lower.equals("unreal.ini") || lower.equals("user.ini"))
				return null;
		}

		StringBuilder sb = new StringBuilder(folder);
		for (int i = 1; i < parts.length; i++)
			sb.append('/').append(parts[i]);
		return sb.toString();
	}

	private static final int SECTOR = 2048;

	// Minimal ISO 9660 reader: uses the Joliet tree (mixed-case long names) when present,
	// otherwise the primary one.
	private static List<IsoFile> listIsoFiles(FileChannel ch) throws IOException
	{
		ByteBuffer primary = null;
		ByteBuffer joliet = null;
		for (int sector = 16; sector < 64; sector++)
		{
			ByteBuffer vd = read(ch, (long)sector * SECTOR, SECTOR);
			int type = vd.get(0) & 0xff;
			if (vd.get(1) != 'C' || vd.get(2) != 'D' || vd.get(3) != '0' || vd.get(4) != '0' || vd.get(5) != '1')
				throw new IOException("Not an ISO 9660 image");
			if (type == 255)
				break;
			if (type == 1 && primary == null)
				primary = vd;
			if (type == 2 && vd.get(88) == '%' && vd.get(89) == '/' && (vd.get(90) == '@' || vd.get(90) == 'C' || vd.get(90) == 'E'))
				joliet = vd;
		}
		ByteBuffer vd = joliet != null ? joliet : primary;
		if (vd == null)
			throw new IOException("No volume descriptor in the disc image");

		long rootExtent = vd.getInt(156 + 2) & 0xffffffffL;
		long rootSize = vd.getInt(156 + 10) & 0xffffffffL;
		List<IsoFile> out = new ArrayList<>();
		walk(ch, rootExtent, rootSize, "", joliet != null, out, 0);
		return out;
	}

	private static void walk(FileChannel ch, long extent, long size, String prefix, boolean joliet, List<IsoFile> out, int depth) throws IOException
	{
		if (depth > 16)
			throw new IOException("Disc image directory tree too deep");
		ByteBuffer dir = read(ch, extent * SECTOR, (int)size);
		int pos = 0;
		while (pos < size)
		{
			int len = dir.get(pos) & 0xff;
			if (len == 0)
			{
				pos = (pos / SECTOR + 1) * SECTOR; // records never cross a sector boundary
				continue;
			}
			long childExtent = dir.getInt(pos + 2) & 0xffffffffL;
			long childSize = dir.getInt(pos + 10) & 0xffffffffL;
			int flags = dir.get(pos + 25) & 0xff;
			int nameLen = dir.get(pos + 32) & 0xff;

			boolean self = nameLen == 1 && (dir.get(pos + 33) == 0 || dir.get(pos + 33) == 1);
			if (!self)
			{
				byte[] raw = new byte[nameLen];
				for (int i = 0; i < nameLen; i++)
					raw[i] = dir.get(pos + 33 + i);
				String name = new String(raw, joliet ? StandardCharsets.UTF_16BE : StandardCharsets.ISO_8859_1);
				int semicolon = name.indexOf(';');
				if (semicolon >= 0)
					name = name.substring(0, semicolon);
				if (!joliet && name.endsWith("."))
					name = name.substring(0, name.length() - 1);

				if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.equals(".."))
					throw new IOException("Bad file name in the disc image");

				if ((flags & 2) != 0)
					walk(ch, childExtent, childSize, prefix + name + "/", joliet, out, depth + 1);
				else
					out.add(new IsoFile(prefix + name, childExtent * SECTOR, childSize));
			}
			pos += len;
		}
	}

	private static ByteBuffer read(FileChannel ch, long offset, int length) throws IOException
	{
		ByteBuffer buf = ByteBuffer.allocate(length).order(ByteOrder.LITTLE_ENDIAN);
		while (buf.hasRemaining())
		{
			if (ch.read(buf, offset + buf.position()) < 0)
				throw new IOException("Unexpected end of the disc image");
		}
		buf.flip();
		return buf;
	}
}
