using System;
using System.IO;
using System.IO.MemoryMappedFiles;

namespace LayerMount.MappedRead;

internal static class Program
{
    private const int PageSize = 4096;

    private const int ExitPatternMatched = 0;
    private const int ExitException = 1;
    private const int ExitPatternMismatch = 2;
    private const int ExitLengthMismatch = 3;
    private const int ExitUsage = 64;

    private readonly record struct Mismatch(long Count, long FirstOffset, byte Expected, byte Actual);

    private static int Main(string[] args)
    {
        if (args.Length != 2 || !long.TryParse(args[1], out long expectedLength) || expectedLength < 0)
        {
            Console.Error.WriteLine("usage: LayerMount.MappedRead <file> <expectedLength>");
            return ExitUsage;
        }

        string path = args[0];
        try
        {
            long actualLength = new FileInfo(path).Length;
            if (actualLength != expectedLength)
            {
                Console.Error.WriteLine(
                    $"Length mismatch: expected {expectedLength} bytes, the file reports {actualLength}");
                return ExitLengthMismatch;
            }

            if (expectedLength == 0)
            {
                return ReadEmptyFile(path);
            }

            return ReadThroughView(path, expectedLength);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"{ex.GetType().Name} HRESULT=0x{ex.HResult:X8}: {ex.Message}");
            return ExitException;
        }
    }

    // A zero-length file cannot be mapped, so the check is that a plain
    // read returns zero bytes.
    private static int ReadEmptyFile(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        byte[] one = new byte[1];
        int read = stream.Read(one, 0, 1);
        if (read != 0)
        {
            Console.Error.WriteLine($"Read {read} byte(s) from a file whose length is 0");
            return ExitPatternMismatch;
        }
        return ExitPatternMatched;
    }

    // A paging read that fails inside the view raises an in-page error
    // that the runtime cannot catch, so the process then dies with that
    // NTSTATUS as its exit code and no line of its own on stderr.
    private static int ReadThroughView(string path, long length)
    {
        using var mapped = MemoryMappedFile.CreateFromFile(
            path, FileMode.Open, null, 0, MemoryMappedFileAccess.Read);
        using var view = mapped.CreateViewAccessor(0, length, MemoryMappedFileAccess.Read);

        Mismatch mismatch = CountMismatches(view, length);
        if (mismatch.Count > 0)
        {
            Console.Error.WriteLine(
                $"Pattern mismatch: {mismatch.Count} byte(s) differ, first at offset {mismatch.FirstOffset} " +
                $"(expected 0x{mismatch.Expected:X2}, read 0x{mismatch.Actual:X2})");
            return ExitPatternMismatch;
        }
        return ExitPatternMatched;
    }

    private static Mismatch CountMismatches(MemoryMappedViewAccessor view, long length)
    {
        byte[] page = new byte[PageSize];
        long count = 0;
        long firstOffset = -1;
        byte expectedAtFirst = 0;
        byte actualAtFirst = 0;

        for (long offset = 0; offset < length; offset += PageSize)
        {
            int read = (int)Math.Min(PageSize, length - offset);
            view.ReadArray(offset, page, 0, read);
            for (int i = 0; i < read; i++)
            {
                byte expected = Pattern.ByteAt(offset + i);
                if (page[i] == expected)
                {
                    continue;
                }
                if (count == 0)
                {
                    firstOffset = offset + i;
                    expectedAtFirst = expected;
                    actualAtFirst = page[i];
                }
                count++;
            }
        }

        return new Mismatch(count, firstOffset, expectedAtFirst, actualAtFirst);
    }
}

internal static class Pattern
{
    public static byte ByteAt(long offset)
    {
        return offset % 64 == 63 ? (byte)0x0A : (byte)(0x21 + (offset % 90));
    }
}
