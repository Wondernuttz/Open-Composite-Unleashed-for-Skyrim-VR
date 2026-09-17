using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;

namespace OpenCompositeConfigurator;

internal readonly record struct BodyTrackerStatusSnapshot(uint Valid, uint Tracked, string Message);

// Only reads the running game's status. Never starts a VR runtime or connects
// to a vendor service. Slot allocation and Gateway process detection are not
// treated as evidence of a tracked pose.
internal static class BodyTrackerMonitor
{
    internal const int RoleCount = 14;
    internal const ulong FreshnessMs = 500;

    internal static int Decode(ulong packed, ulong now)
    {
        ulong then = packed >> 2;
        if (packed == 0 || now < then || now - then > FreshnessMs || (packed & 1) == 0)
            return 0;
        return (packed & 2) != 0 ? 2 : 1;
    }

    internal static BodyTrackerStatusSnapshot Read()
    {
        bool gameFound = false;
        foreach (var process in Process.GetProcessesByName("SkyrimVR"))
        {
            using (process)
            {
                gameFound = true;
                var status = ReadProcess(process);
                if (status.HasValue) return status.Value;
            }
        }
        return new(0, 0, gameFound ? "Game running — waiting for tracker poses" : "Start Skyrim to check body poses");
    }

    internal static BodyTrackerStatusSnapshot? ReadProcess(Process process)
    {
        try
        {
                    using var map = MemoryMappedFile.OpenExisting(
                        $"Local\\OCU.BodyTrackers.v1.{process.Id}", MemoryMappedFileRights.Read);
                    using var view = map.CreateViewAccessor(0, 16 + RoleCount * 8, MemoryMappedFileAccess.Read);
                    if (view.ReadUInt32(0) != 0x5354434f || view.ReadUInt32(4) != 1
                        || view.ReadUInt32(8) != (uint)process.Id || view.ReadUInt32(12) != RoleCount)
                        return null;
                    uint valid = 0, tracked = 0;
                    ulong now = (ulong)Environment.TickCount64;
                    for (int i = 0; i < RoleCount; ++i)
                    {
                        int state = Decode(view.ReadUInt64(16 + i * 8), now);
                        if (state != 0) valid |= 1u << i;
                        if (state == 2) tracked |= 1u << i;
                    }
                    if (process.HasExited || view.ReadUInt32(0) != 0x5354434f) return null;
                    return new BodyTrackerStatusSnapshot(valid, tracked, valid == 0
                        ? "Game running — no fresh body poses"
                        : "Body poses received from the game");
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException
            || e is InvalidOperationException || e is ArgumentException
            || e is System.ComponentModel.Win32Exception) { }
        return null;
    }
}
