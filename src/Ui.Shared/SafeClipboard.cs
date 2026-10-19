using System.Runtime.InteropServices;

namespace OnimushaPS5Adapter.Ui;

internal static class SafeClipboard
{
    private const int MaxAttempts = 8;
    private const int BaseDelayMilliseconds = 35;
    private const uint ClipboardFormatUnicodeText = 13;
    private const uint GlobalMemoryMoveable = 0x0002;
    private const uint GlobalMemoryZeroInitialize = 0x0040;
    private const int ClipboardCannotOpenHResult = unchecked((int)0x800401D0);

    public static bool TrySetText(string? text, out string errorMessage)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            errorMessage = "没有可复制的内容。";
            return false;
        }

        return TryExecute(() => SetTextNative(text), out errorMessage);
    }

    public static bool TryGetText(out string text, out string errorMessage)
    {
        var capturedText = string.Empty;
        if (!TryExecute(() => capturedText = GetTextNative(), out errorMessage))
        {
            text = string.Empty;
            return false;
        }

        text = capturedText;
        if (!string.IsNullOrWhiteSpace(text))
            return true;

        errorMessage = "剪贴板中没有可用的文本。";
        return false;
    }

    private static bool TryExecute(Action operation, out string errorMessage)
    {
        Exception? lastTransientException = null;
        for (var attempt = 0; attempt < MaxAttempts; attempt++)
        {
            try
            {
                operation();
                errorMessage = string.Empty;
                return true;
            }
            catch (ExternalException exception)
            {
                lastTransientException = exception;
                if (attempt + 1 < MaxAttempts)
                    Thread.Sleep(BaseDelayMilliseconds * (attempt + 1));
            }
            catch (ThreadStateException)
            {
                errorMessage = "剪贴板操作线程状态异常，请重新打开程序后再试。";
                return false;
            }
            catch (Exception exception)
            {
                errorMessage = "剪贴板操作失败：" + exception.Message;
                return false;
            }
        }

        errorMessage = lastTransientException is null
            ? "剪贴板操作失败，请稍后重试。"
            : "Windows 剪贴板正被其他程序占用，请稍后重试。";
        return false;
    }

    private static void SetTextNative(string text)
    {
        OpenClipboardOrThrow();
        IntPtr globalMemory = IntPtr.Zero;
        try
        {
            if (!NativeMethods.EmptyClipboard())
                ThrowClipboardFailure("无法清空 Windows 剪贴板。");

            var byteCount = checked((text.Length + 1) * sizeof(char));
            globalMemory = NativeMethods.GlobalAlloc(
                GlobalMemoryMoveable | GlobalMemoryZeroInitialize,
                (UIntPtr)(uint)byteCount);
            if (globalMemory == IntPtr.Zero)
                throw new OutOfMemoryException("无法为剪贴板文本分配内存。");

            var buffer = NativeMethods.GlobalLock(globalMemory);
            if (buffer == IntPtr.Zero)
                ThrowClipboardFailure("无法写入 Windows 剪贴板内存。");

            try
            {
                var characters = text.ToCharArray();
                Marshal.Copy(characters, 0, buffer, characters.Length);
            }
            finally
            {
                NativeMethods.GlobalUnlock(globalMemory);
            }

            if (NativeMethods.SetClipboardData(ClipboardFormatUnicodeText, globalMemory) == IntPtr.Zero)
                ThrowClipboardFailure("无法设置 Windows 剪贴板文本。");

            // SetClipboardData 成功后，内存所有权已转交给系统。
            globalMemory = IntPtr.Zero;
        }
        finally
        {
            if (globalMemory != IntPtr.Zero)
                NativeMethods.GlobalFree(globalMemory);

            NativeMethods.CloseClipboard();
        }
    }

    private static string GetTextNative()
    {
        OpenClipboardOrThrow();
        try
        {
            if (!NativeMethods.IsClipboardFormatAvailable(ClipboardFormatUnicodeText))
                return string.Empty;

            var globalMemory = NativeMethods.GetClipboardData(ClipboardFormatUnicodeText);
            if (globalMemory == IntPtr.Zero)
                ThrowClipboardFailure("无法读取 Windows 剪贴板文本。");

            var buffer = NativeMethods.GlobalLock(globalMemory);
            if (buffer == IntPtr.Zero)
                ThrowClipboardFailure("无法读取 Windows 剪贴板内存。");

            try
            {
                return Marshal.PtrToStringUni(buffer) ?? string.Empty;
            }
            finally
            {
                NativeMethods.GlobalUnlock(globalMemory);
            }
        }
        finally
        {
            NativeMethods.CloseClipboard();
        }
    }

    private static void OpenClipboardOrThrow()
    {
        if (!NativeMethods.OpenClipboard(IntPtr.Zero))
            ThrowClipboardFailure("无法打开 Windows 剪贴板。");
    }

    private static void ThrowClipboardFailure(string message)
    {
        var errorCode = Marshal.GetLastWin32Error();
        var hResult = errorCode == 0
            ? ClipboardCannotOpenHResult
            : Marshal.GetHRForLastWin32Error();
        throw new ExternalException(message, hResult);
    }

    private static class NativeMethods
    {
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool OpenClipboard(IntPtr newOwner);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CloseClipboard();

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool EmptyClipboard();

        [DllImport("user32.dll", SetLastError = true)]
        internal static extern IntPtr SetClipboardData(uint format, IntPtr memory);

        [DllImport("user32.dll", SetLastError = true)]
        internal static extern IntPtr GetClipboardData(uint format);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool IsClipboardFormatAvailable(uint format);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern IntPtr GlobalLock(IntPtr memory);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GlobalUnlock(IntPtr memory);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern IntPtr GlobalFree(IntPtr memory);
    }
}
