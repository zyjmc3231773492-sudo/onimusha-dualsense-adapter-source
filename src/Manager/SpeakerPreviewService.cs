using System.Buffers.Binary;
using System.IO;
using System.IO.Pipes;

namespace OnimushaPS5Adapter.Manager;

internal static class SpeakerPreviewService
{
    internal static async Task<byte> PlayAsync(int gameProcessId, byte cue, int volume)
    {
        if (cue is not (2 or 4 or 5 or 6 or 7 or 11 or 12 or 13 or 14) || volume is < 0 or > 200)
            throw new ArgumentOutOfRangeException(nameof(cue));
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(2));
        using var pipe = new NamedPipeClientStream(".",
            $"CodexOnimushaDualSense_{gameProcessId}_SpeakerPreview",
            PipeDirection.InOut, PipeOptions.Asynchronous);
        await pipe.ConnectAsync(deadline.Token);
        byte[] request = [(byte)'S', (byte)'P', 1, cue, 0, 0, 0, 0];
        BinaryPrimitives.WriteInt32LittleEndian(request.AsSpan(4), volume);
        await pipe.WriteAsync(request, deadline.Token);
        byte[] response = new byte[8];
        await pipe.ReadExactlyAsync(response, deadline.Token);
        if (response[0] != 'S' || response[1] != 'P' || response[2] != 1 ||
            response[3] is < 1 or > 6 || response[4] != cue || response[5] != volume)
            throw new InvalidDataException(UiText.Get("TheAudioPreviewResponseFormatIsIncompatible"));
        return response[3];
    }
}
