using System.IO;
using System.Windows;
using System.Windows.Media.Imaging;

namespace OnimushaPS5Adapter.Manager;

public partial class ContactQrWindow : Window
{
    public ContactQrWindow()
    {
        InitializeComponent();
        Loaded += (_, _) => LoadImages();
    }

    private void LoadImages()
    {
        var baseDirectory = Path.GetDirectoryName(typeof(ContactQrWindow).Assembly.Location)
            ?? AppContext.BaseDirectory;
        LoadImage(Path.Combine(baseDirectory, "assets", "wechat-contact.jpg"), WechatContactImage, WechatMissingText);
        LoadQqImage(Path.Combine(baseDirectory, "assets", "qq-update-group.jpg"));
    }

    private static void LoadImage(string path, System.Windows.Controls.Image image, UIElement placeholder)
    {
        if (!File.Exists(path))
            return;

        var bitmap = LoadBitmap(path);
        image.Source = bitmap;
        placeholder.Visibility = Visibility.Collapsed;
    }

    private void LoadQqImage(string path)
    {
        if (!File.Exists(path))
            return;

        var bitmap = LoadBitmap(path);
        // The supplied QQ image is a tall phone screenshot. Crop only the empty outer area at
        // display time so the original QR pixels remain untouched and large enough to scan.
        var cropX = Math.Min(70, Math.Max(0, bitmap.PixelWidth - 1));
        var cropY = Math.Min(250, Math.Max(0, bitmap.PixelHeight - 1));
        var cropWidth = Math.Min(1140, bitmap.PixelWidth - cropX);
        var cropHeight = Math.Min(1460, bitmap.PixelHeight - cropY);
        QqGroupImage.Source = cropWidth > 0 && cropHeight > 0
            ? new CroppedBitmap(bitmap, new Int32Rect(cropX, cropY, cropWidth, cropHeight))
            : bitmap;
        QqMissingText.Visibility = Visibility.Collapsed;
    }

    private static BitmapImage LoadBitmap(string path)
    {
        var bitmap = new BitmapImage();
        bitmap.BeginInit();
        bitmap.CacheOption = BitmapCacheOption.OnLoad;
        bitmap.UriSource = new Uri(path, UriKind.Absolute);
        bitmap.EndInit();
        bitmap.Freeze();
        return bitmap;
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
