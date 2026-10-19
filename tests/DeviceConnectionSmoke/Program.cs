using OnimushaPS5Adapter.Manager;
using System.Text.Json;

var snapshot = DeviceConnectionService.GetSnapshot();
Console.WriteLine(JsonSerializer.Serialize(snapshot));
if (!snapshot.DetectionAvailable) throw new Exception(snapshot.Detail);
if (snapshot.IsConnected && !snapshot.Status.Contains("HID"))
    throw new Exception("Connected HID state missing");
Console.WriteLine("Windows HID enumeration and attribute-based controller detection passed.");
