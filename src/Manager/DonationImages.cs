using System.IO;
using System.Windows;
using System.Windows.Media.Imaging;

namespace OnimushaPS5Adapter.Manager;

internal static class DonationImages
{
    internal static BitmapImage? LoadOriginal(string provider)
    {
        if (provider is not ("alipay" or "wechat")) return null;
        var directory = Path.GetDirectoryName(typeof(DonationImages).Assembly.Location) ?? AppContext.BaseDirectory;
        var path = Path.Combine(directory, "assets", $"donate-{provider}.jpg");
        if (!File.Exists(path)) return null;
        try
        {
            var bitmap = new BitmapImage();
            bitmap.BeginInit();
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.UriSource = new Uri(path, UriKind.Absolute);
            bitmap.EndInit();
            bitmap.Freeze();
            return bitmap;
        }
        catch (Exception exception) when (exception is IOException or NotSupportedException)
        {
            System.Diagnostics.Debug.WriteLine(exception.Message);
            return null;
        }
    }

    internal static void LoadThumbnail(string provider, System.Windows.Controls.Image image, UIElement placeholder)
    {
        var bitmap = LoadOriginal(provider);
        if (bitmap is null) return;
        // Only the WPF view is cropped. Bundled payment images and their QR pixels stay untouched.
        // Include the full QR quiet zone so the inline image remains scannable.
        var bounds = provider == "alipay"
            ? (X: .247, Y: .270, Width: .507, Height: .323)
            : (X: .270, Y: .243, Width: .460, Height: .340);
        image.Source = new CroppedBitmap(bitmap, new Int32Rect(
            (int)(bitmap.PixelWidth * bounds.X), (int)(bitmap.PixelHeight * bounds.Y),
            (int)(bitmap.PixelWidth * bounds.Width), (int)(bitmap.PixelHeight * bounds.Height)));
        placeholder.Visibility = Visibility.Collapsed;
    }
}
