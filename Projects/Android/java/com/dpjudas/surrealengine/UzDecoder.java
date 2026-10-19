package com.dpjudas.surrealengine;

import java.io.IOException;
import java.nio.charset.StandardCharsets;

// Decoder for Unreal Engine 1 ".uz" files (the compressed maps on the Unreal Tournament disc).
//
// Format, as written by "ucc compress" in UT: an int signature (5678; 1234 in older builds), the original file name as
// an FString, then the data passed through five codecs - RLE, Burrows-Wheeler, move-to-front,
// RLE, Huffman - so decoding runs them in reverse. Written from the codec description in Epic's
// public UE1 headers (Core/Inc/FCodec.h); everything is little-endian.
final class UzDecoder
{
	static final int SIGNATURE_UT = 5678;
	static final int SIGNATURE_OLD = 1234;

	static final class Result
	{
		final String fileName;
		final byte[] data;

		Result(String fileName, byte[] data)
		{
			this.fileName = fileName;
			this.data = data;
		}
	}

	static Result decode(byte[] uz) throws IOException
	{
		int[] pos = {0};
		int signature = readInt(uz, pos);
		if (signature != SIGNATURE_UT && signature != SIGNATURE_OLD)
			throw new IOException("Not a UT .uz file (signature " + signature + ")");

		int nameLen = readCompactIndex(uz, pos);
		if (nameLen <= 0 || nameLen > 1024 || pos[0] + nameLen > uz.length)
			throw new IOException("Bad file name in .uz header");
		String name = new String(uz, pos[0], nameLen - 1, StandardCharsets.ISO_8859_1); // without the terminating 0
		pos[0] += nameLen;

		byte[] data = huffmanDecode(uz, pos[0]);
		data = rleDecode(data);
		data = mtfDecode(data);
		data = bwtDecode(data);
		data = rleDecode(data);
		return new Result(name, data);
	}

	private static int readInt(byte[] b, int[] pos) throws IOException
	{
		int p = pos[0];
		if (p + 4 > b.length)
			throw new IOException("Unexpected end of .uz data");
		pos[0] = p + 4;
		return (b[p] & 0xff) | ((b[p + 1] & 0xff) << 8) | ((b[p + 2] & 0xff) << 16) | ((b[p + 3] & 0xff) << 24);
	}

	// FCompactIndex: first byte = sign (0x80), more (0x40), 6 value bits; then bytes of 7 value bits + more (0x80).
	private static int readCompactIndex(byte[] b, int[] pos) throws IOException
	{
		if (pos[0] >= b.length)
			throw new IOException("Unexpected end of .uz data");
		int b0 = b[pos[0]++] & 0xff;
		int value = b0 & 0x3f;
		boolean more = (b0 & 0x40) != 0;
		int shift = 6;
		while (more && shift < 32)
		{
			if (pos[0] >= b.length)
				throw new IOException("Unexpected end of .uz data");
			int bn = b[pos[0]++] & 0xff;
			value |= (bn & 0x7f) << shift;
			more = (bn & 0x80) != 0;
			shift += 7;
		}
		return (b0 & 0x80) != 0 ? -value : value;
	}

	// ---- Huffman: int total, then an LSB-first bitstream holding the tree and the symbols ----

	private static byte[] huffmanDecode(byte[] in, int start) throws IOException
	{
		int[] pos = {start};
		int total = readInt(in, pos);
		if (total < 0 || total > 256 * 1024 * 1024)
			throw new IOException("Bad .uz Huffman length");

		BitReader reader = new BitReader(in, pos[0]);
		// Tree as arrays: node i has children left[i]/right[i], or a symbol (sym[i] >= 0) if a leaf.
		int[] left = new int[511], right = new int[511], sym = new int[511];
		int[] count = {0};
		readTable(reader, left, right, sym, count);

		byte[] out = new byte[total];
		for (int i = 0; i < total; i++)
		{
			int node = 0;
			while (sym[node] < 0)
				node = reader.readBit() ? right[node] : left[node];
			out[i] = (byte)sym[node];
		}
		return out;
	}

	private static int readTable(BitReader reader, int[] left, int[] right, int[] sym, int[] count) throws IOException
	{
		int node = count[0]++;
		if (node >= sym.length)
			throw new IOException("Bad .uz Huffman table");
		if (reader.readBit())
		{
			sym[node] = -1;
			left[node] = readTable(reader, left, right, sym, count);
			right[node] = readTable(reader, left, right, sym, count);
		}
		else
		{
			int b = 0;
			for (int i = 0; i < 8; i++)
				if (reader.readBit())
					b |= 1 << i;
			sym[node] = b;
		}
		return node;
	}

	private static final class BitReader
	{
		private final byte[] data;
		private long bit;
		private final long end;

		BitReader(byte[] data, int startByte)
		{
			this.data = data;
			this.bit = (long)startByte * 8;
			this.end = (long)data.length * 8;
		}

		boolean readBit() throws IOException
		{
			if (bit >= end)
				throw new IOException("Unexpected end of .uz bitstream");
			boolean b = (data[(int)(bit >> 3)] & (1 << (bit & 7))) != 0;
			bit++;
			return b;
		}
	}

	// ---- RLE: after 5 equal bytes comes a count byte; the run is that long in total ----

	private static byte[] rleDecode(byte[] in) throws IOException
	{
		ByteSink out = new ByteSink(in.length * 2 + 16);
		int count = 0;
		int prev = 0;
		int i = 0;
		while (i < in.length)
		{
			int b = in[i++] & 0xff;
			out.put(b);
			if (b != prev)
			{
				prev = b;
				count = 1;
			}
			else if (++count == 5)
			{
				if (i >= in.length)
					throw new IOException("Unexpected end of .uz RLE data");
				int c = in[i++] & 0xff;
				if (c < 2)
					throw new IOException("Bad .uz RLE run");
				for (int n = c - 5; n > 0; n--)
					out.put(b);
				count = 0;
			}
		}
		return out.toArray();
	}

	// ---- Move-to-front ----

	private static byte[] mtfDecode(byte[] in)
	{
		byte[] list = new byte[256];
		for (int i = 0; i < 256; i++)
			list[i] = (byte)i;
		byte[] out = new byte[in.length];
		for (int k = 0; k < in.length; k++)
		{
			int idx = in[k] & 0xff;
			byte c = list[idx];
			out[k] = c;
			System.arraycopy(list, 0, list, 1, idx);
			list[0] = c;
		}
		return out;
	}

	// ---- Burrows-Wheeler: blocks of {int length, int first, int last, length+1 bytes} ----

	private static final int BWT_MAX_BUFFER = 0x40000;

	private static byte[] bwtDecode(byte[] in) throws IOException
	{
		ByteSink out = new ByteSink(in.length);
		int[] temp = new int[BWT_MAX_BUFFER + 1];
		int[] counts = new int[257];
		int[] running = new int[257];
		int[] pos = {0};
		while (pos[0] < in.length)
		{
			int length = readInt(in, pos);
			int first = readInt(in, pos);
			int last = readInt(in, pos);
			if (length < 0 || length > BWT_MAX_BUFFER || pos[0] + length + 1 > in.length)
				throw new IOException("Bad .uz BWT block");
			int n = length + 1;
			int base = pos[0];
			pos[0] += n;
			if (first < 0 || first >= n || last < 0 || last >= n)
				throw new IOException("Bad .uz BWT block index");

			java.util.Arrays.fill(counts, 0);
			for (int i = 0; i < n; i++)
				counts[i != last ? (in[base + i] & 0xff) : 256]++;
			int sum = 0;
			for (int i = 0; i < 257; i++)
			{
				running[i] = sum;
				sum += counts[i];
				counts[i] = 0;
			}
			for (int i = 0; i < n; i++)
			{
				int index = i != last ? (in[base + i] & 0xff) : 256;
				temp[running[index] + counts[index]++] = i;
			}
			for (int i = first, j = 0; j < n - 1; i = temp[i], j++)
				out.put(in[base + i] & 0xff);
		}
		return out.toArray();
	}

	private static final class ByteSink
	{
		private byte[] buf;
		private int size;

		ByteSink(int capacity)
		{
			buf = new byte[Math.max(capacity, 64)];
		}

		void put(int b)
		{
			if (size == buf.length)
				buf = java.util.Arrays.copyOf(buf, buf.length * 2);
			buf[size++] = (byte)b;
		}

		byte[] toArray()
		{
			return java.util.Arrays.copyOf(buf, size);
		}
	}
}
