using System;
using System.Globalization;
using System.IO;
using System.Numerics;
using System.Text;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>Labels selected by the operator for one calibration segment.</summary>
    public readonly struct World3DCalibrationLabels
    {
        public World3DCalibrationLabels(
            int segment,
            string? view,
            string? action,
            string? leg,
            int take)
        {
            if (segment < 0)
                throw new ArgumentOutOfRangeException(nameof(segment));
            if (take < 0)
                throw new ArgumentOutOfRangeException(nameof(take));

            Segment = segment;
            View = view ?? string.Empty;
            Action = action ?? string.Empty;
            Leg = leg ?? string.Empty;
            Take = take;
        }

        public int Segment { get; }
        public string View { get; }
        public string Action { get; }
        public string Leg { get; }
        public int Take { get; }
    }

    /// <summary>
    /// Caller-supplied diagnostics for one leg. These values are intentionally
    /// independent of a particular classifier so raw World3D captures remain
    /// usable while gait/kick state logic evolves.
    /// </summary>
    public readonly struct World3DLegCaptureFields
    {
        public World3DLegCaptureFields(
            bool isValid = false,
            int stateCode = 0,
            string? stateLabel = null,
            string? stateSource = null,
            long stateTimestampMs = 0,
            float stateConfidence = 0f,
            bool baselineReady = false,
            bool blocksGait = false,
            bool stepOnset = false,
            bool anatomicalFlipRejected = false,
            bool groundReferenceValid = false,
            float groundReferenceYMeters = 0f,
            float kneeAngleDegrees = 0f,
            float straightness = 0f,
            float hipKneeLengthMeters = 0f,
            float kneeAnkleLengthMeters = 0f,
            float ankleLiftMeters = 0f,
            float footLiftMeters = 0f,
            float horizontalShinMeters = 0f,
            bool strikeBearingValid = false,
            Vector3 strikeBearing = default,
            float strikeWeight = 0f,
            bool hipVelocityValid = false,
            Vector3 hipVelocityMetersPerSecond = default,
            bool kneeVelocityValid = false,
            Vector3 kneeVelocityMetersPerSecond = default,
            bool ankleVelocityValid = false,
            Vector3 ankleVelocityMetersPerSecond = default,
            bool footVelocityValid = false,
            Vector3 footVelocityMetersPerSecond = default)
        {
            IsValid = isValid;
            StateCode = stateCode;
            StateLabel = stateLabel ?? string.Empty;
            StateSource = stateSource ?? string.Empty;
            StateTimestampMs = stateTimestampMs;
            StateConfidence = stateConfidence;
            BaselineReady = baselineReady;
            BlocksGait = blocksGait;
            StepOnset = stepOnset;
            AnatomicalFlipRejected = anatomicalFlipRejected;
            GroundReferenceValid = groundReferenceValid;
            GroundReferenceYMeters = groundReferenceYMeters;
            KneeAngleDegrees = kneeAngleDegrees;
            Straightness = straightness;
            HipKneeLengthMeters = hipKneeLengthMeters;
            KneeAnkleLengthMeters = kneeAnkleLengthMeters;
            AnkleLiftMeters = ankleLiftMeters;
            FootLiftMeters = footLiftMeters;
            HorizontalShinMeters = horizontalShinMeters;
            StrikeBearingValid = strikeBearingValid;
            StrikeBearing = strikeBearing;
            StrikeWeight = strikeWeight;
            HipVelocityValid = hipVelocityValid;
            HipVelocityMetersPerSecond = hipVelocityMetersPerSecond;
            KneeVelocityValid = kneeVelocityValid;
            KneeVelocityMetersPerSecond = kneeVelocityMetersPerSecond;
            AnkleVelocityValid = ankleVelocityValid;
            AnkleVelocityMetersPerSecond = ankleVelocityMetersPerSecond;
            FootVelocityValid = footVelocityValid;
            FootVelocityMetersPerSecond = footVelocityMetersPerSecond;
        }

        public bool IsValid { get; }
        public int StateCode { get; }
        public string StateLabel { get; }
        public string StateSource { get; }
        public long StateTimestampMs { get; }
        public float StateConfidence { get; }
        public bool BaselineReady { get; }
        public bool BlocksGait { get; }
        public bool StepOnset { get; }
        public bool AnatomicalFlipRejected { get; }
        public bool GroundReferenceValid { get; }
        public float GroundReferenceYMeters { get; }
        public float KneeAngleDegrees { get; }
        public float Straightness { get; }
        public float HipKneeLengthMeters { get; }
        public float KneeAnkleLengthMeters { get; }
        public float AnkleLiftMeters { get; }
        public float FootLiftMeters { get; }
        public float HorizontalShinMeters { get; }
        public bool StrikeBearingValid { get; }
        public Vector3 StrikeBearing { get; }
        public float StrikeWeight { get; }
        public bool HipVelocityValid { get; }
        public Vector3 HipVelocityMetersPerSecond { get; }
        public bool KneeVelocityValid { get; }
        public Vector3 KneeVelocityMetersPerSecond { get; }
        public bool AnkleVelocityValid { get; }
        public Vector3 AnkleVelocityMetersPerSecond { get; }
        public bool FootVelocityValid { get; }
        public Vector3 FootVelocityMetersPerSecond { get; }
    }

    /// <summary>One deterministic row passed to the CSV serializer.</summary>
    public sealed class World3DCalibrationRecord
    {
        public World3DCalibrationRecord(
            long captureTimestampMs,
            long captureElapsedMs,
            DateTimeOffset utcTimestamp,
            World3DCalibrationLabels labels,
            WorldLandmarkFrame landmarks,
            BodyTrackerFrame? convertedFrame,
            World3DLegCaptureFields leftLeg,
            World3DLegCaptureFields rightLeg)
        {
            ArgumentNullException.ThrowIfNull(landmarks);
            if (captureElapsedMs < 0)
                throw new ArgumentOutOfRangeException(nameof(captureElapsedMs));

            CaptureTimestampMs = captureTimestampMs;
            CaptureElapsedMs = captureElapsedMs;
            UtcTimestamp = utcTimestamp;
            Labels = labels;
            Landmarks = landmarks;
            ConvertedFrame = convertedFrame;
            LeftLeg = leftLeg;
            RightLeg = rightLeg;
        }

        public long CaptureTimestampMs { get; }
        public long CaptureElapsedMs { get; }
        public DateTimeOffset UtcTimestamp { get; }
        public World3DCalibrationLabels Labels { get; }
        public WorldLandmarkFrame Landmarks { get; }
        public BodyTrackerFrame? ConvertedFrame { get; }
        public World3DLegCaptureFields LeftLeg { get; }
        public World3DLegCaptureFields RightLeg { get; }
    }

    /// <summary>
    /// Stable, invariant CSV schema for raw MediaPipe World3D calibration data.
    /// The raw frame is written before any OCU coordinate conversion or filter;
    /// converted head/tracker poses are included alongside it for comparison.
    /// </summary>
    public static class World3DCalibrationCsvSerializer
    {
        private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;

        public static void WriteHeader(TextWriter writer)
        {
            ArgumentNullException.ThrowIfNull(writer);
            var line = new StringBuilder(16 * 1024);

            Add(line, "capture_monotonic_ms");
            Add(line, "capture_elapsed_ms");
            Add(line, "utc_iso8601");
            Add(line, "segment_id");
            Add(line, "view_label");
            Add(line, "action_label");
            Add(line, "leg_label");
            Add(line, "take");
            Add(line, "source_sequence");
            Add(line, "source_timestamp_ms");
            Add(line, "source_epoch");
            Add(line, "source_flags");
            Add(line, "image_width");
            Add(line, "image_height");
            Add(line, "input_mirrored");

            for (int i = 0; i < WorldLandmarkFrame.MediaPipeLandmarkCount; i++)
            {
                string stem = LandmarkStem(i);
                Add(line, stem + "_world_x_m");
                Add(line, stem + "_world_y_m");
                Add(line, stem + "_world_z_m");
                Add(line, stem + "_confidence");
                Add(line, stem + "_valid");
                Add(line, stem + "_image_x_norm");
                Add(line, stem + "_image_y_norm");
                Add(line, stem + "_image_z_model");
            }

            Add(line, "converted_present");
            Add(line, "converted_sequence");
            Add(line, "converted_timestamp_ms");
            Add(line, "converted_epoch");
            Add(line, "converted_source");
            Add(line, "converted_source_flags");
            Add(line, "converted_core_pose_valid");
            Add(line, "converted_pose_mask");
            Add(line, "converted_matches_source");

            AddPoseHeader(line, "head");
            for (int slot = 1; slot <= BodyTrackerFrame.TrackerSlotCount; slot++)
                AddPoseHeader(line, $"tracker_{slot:00}");

            AddLegHeader(line, "left");
            AddLegHeader(line, "right");
            writer.WriteLine(line.ToString());
        }

        public static void WriteRow(TextWriter writer, World3DCalibrationRecord record)
        {
            ArgumentNullException.ThrowIfNull(writer);
            ArgumentNullException.ThrowIfNull(record);

            WorldLandmarkFrame source = record.Landmarks;
            var line = new StringBuilder(24 * 1024);
            Add(line, record.CaptureTimestampMs);
            Add(line, record.CaptureElapsedMs);
            Add(line, record.UtcTimestamp.UtcDateTime.ToString("O", Invariant));
            Add(line, record.Labels.Segment);
            Add(line, record.Labels.View);
            Add(line, record.Labels.Action);
            Add(line, record.Labels.Leg);
            Add(line, record.Labels.Take);
            Add(line, source.Sequence);
            Add(line, source.TimestampMs);
            Add(line, source.SourceEpoch);
            Add(line, (uint)source.SourceFlags);
            Add(line, source.ImageWidth);
            Add(line, source.ImageHeight);
            Add(line, source.InputMirrored);

            for (int i = 0; i < source.LandmarkCount; i++)
            {
                WorldLandmark landmark = source[i];
                Add(line, landmark.WorldPosition.X);
                Add(line, landmark.WorldPosition.Y);
                Add(line, landmark.WorldPosition.Z);
                Add(line, landmark.Confidence);
                Add(line, landmark.IsValid);
                Add(line, landmark.NormalizedPosition.X);
                Add(line, landmark.NormalizedPosition.Y);
                Add(line, landmark.NormalizedPosition.Z);
            }

            BodyTrackerFrame? converted = record.ConvertedFrame;
            Add(line, converted != null);
            if (converted == null)
            {
                AddEmpty(line, 8);
                AddEmptyPose(line);
                for (int slot = 1; slot <= BodyTrackerFrame.TrackerSlotCount; slot++)
                    AddEmptyPose(line);
            }
            else
            {
                Add(line, converted.Sequence);
                Add(line, converted.TimestampMs);
                Add(line, converted.SourceEpoch);
                Add(line, (int)converted.Source);
                Add(line, (uint)converted.SourceFlags);
                Add(line, converted.CorePoseValid);
                Add(line, converted.OscPoseMask);
                Add(line, converted.Sequence == source.Sequence
                    && converted.SourceEpoch == source.SourceEpoch);
                AddPose(line, converted.Head);
                for (int slot = 1; slot <= BodyTrackerFrame.TrackerSlotCount; slot++)
                    AddPose(line, converted.GetTracker(slot));
            }

            AddLeg(line, record.LeftLeg);
            AddLeg(line, record.RightLeg);
            writer.WriteLine(line.ToString());
        }

        private static void AddPoseHeader(StringBuilder line, string prefix)
        {
            Add(line, prefix + "_position_valid");
            Add(line, prefix + "_position_x_m");
            Add(line, prefix + "_position_y_m");
            Add(line, prefix + "_position_z_m");
            Add(line, prefix + "_rotation_valid");
            Add(line, prefix + "_rotation_x");
            Add(line, prefix + "_rotation_y");
            Add(line, prefix + "_rotation_z");
            Add(line, prefix + "_rotation_w");
            Add(line, prefix + "_confidence");
        }

        private static void AddPose(StringBuilder line, BodyTrackerPose pose)
        {
            Add(line, pose.PositionValid);
            Add(line, pose.Position.X);
            Add(line, pose.Position.Y);
            Add(line, pose.Position.Z);
            Add(line, pose.RotationValid);
            Add(line, pose.Orientation.X);
            Add(line, pose.Orientation.Y);
            Add(line, pose.Orientation.Z);
            Add(line, pose.Orientation.W);
            Add(line, pose.Confidence);
        }

        private static void AddEmptyPose(StringBuilder line) => AddEmpty(line, 10);

        private static void AddLegHeader(StringBuilder line, string prefix)
        {
            Add(line, prefix + "_valid");
            Add(line, prefix + "_state_code");
            Add(line, prefix + "_state_label");
            Add(line, prefix + "_state_source");
            Add(line, prefix + "_state_timestamp_ms");
            Add(line, prefix + "_state_confidence");
            Add(line, prefix + "_baseline_ready");
            Add(line, prefix + "_blocks_gait");
            Add(line, prefix + "_step_onset");
            Add(line, prefix + "_anatomical_flip_rejected");
            Add(line, prefix + "_ground_reference_valid");
            Add(line, prefix + "_ground_reference_y_m");
            Add(line, prefix + "_knee_angle_deg");
            Add(line, prefix + "_straightness");
            Add(line, prefix + "_hip_knee_length_m");
            Add(line, prefix + "_knee_ankle_length_m");
            Add(line, prefix + "_ankle_lift_m");
            Add(line, prefix + "_foot_lift_m");
            Add(line, prefix + "_horizontal_shin_m");
            Add(line, prefix + "_strike_bearing_valid");
            Add(line, prefix + "_strike_bearing_x");
            Add(line, prefix + "_strike_bearing_y");
            Add(line, prefix + "_strike_bearing_z");
            Add(line, prefix + "_strike_weight");
            Add(line, prefix + "_hip_velocity_valid");
            AddVectorHeader(line, prefix + "_hip_velocity_mps");
            Add(line, prefix + "_knee_velocity_valid");
            AddVectorHeader(line, prefix + "_knee_velocity_mps");
            Add(line, prefix + "_ankle_velocity_valid");
            AddVectorHeader(line, prefix + "_ankle_velocity_mps");
            Add(line, prefix + "_foot_velocity_valid");
            AddVectorHeader(line, prefix + "_foot_velocity_mps");
        }

        private static void AddLeg(StringBuilder line, World3DLegCaptureFields leg)
        {
            Add(line, leg.IsValid);
            Add(line, leg.StateCode);
            Add(line, leg.StateLabel);
            Add(line, leg.StateSource);
            Add(line, leg.StateTimestampMs);
            Add(line, leg.StateConfidence);
            Add(line, leg.BaselineReady);
            Add(line, leg.BlocksGait);
            Add(line, leg.StepOnset);
            Add(line, leg.AnatomicalFlipRejected);
            Add(line, leg.GroundReferenceValid);
            Add(line, leg.GroundReferenceYMeters);
            Add(line, leg.KneeAngleDegrees);
            Add(line, leg.Straightness);
            Add(line, leg.HipKneeLengthMeters);
            Add(line, leg.KneeAnkleLengthMeters);
            Add(line, leg.AnkleLiftMeters);
            Add(line, leg.FootLiftMeters);
            Add(line, leg.HorizontalShinMeters);
            Add(line, leg.StrikeBearingValid);
            Add(line, leg.StrikeBearing.X);
            Add(line, leg.StrikeBearing.Y);
            Add(line, leg.StrikeBearing.Z);
            Add(line, leg.StrikeWeight);
            Add(line, leg.HipVelocityValid);
            Add(line, leg.HipVelocityMetersPerSecond);
            Add(line, leg.KneeVelocityValid);
            Add(line, leg.KneeVelocityMetersPerSecond);
            Add(line, leg.AnkleVelocityValid);
            Add(line, leg.AnkleVelocityMetersPerSecond);
            Add(line, leg.FootVelocityValid);
            Add(line, leg.FootVelocityMetersPerSecond);
        }

        private static void AddVectorHeader(StringBuilder line, string prefix)
        {
            Add(line, prefix + "_x");
            Add(line, prefix + "_y");
            Add(line, prefix + "_z");
        }

        private static string LandmarkStem(int index)
        {
            string name = ((MediaPipePoseLandmarkIndex)index).ToString();
            var result = new StringBuilder(name.Length + 8);
            result.Append("mp_").Append(index.ToString("00", Invariant)).Append('_');
            for (int i = 0; i < name.Length; i++)
            {
                char current = name[i];
                if (i > 0 && char.IsUpper(current))
                    result.Append('_');
                result.Append(char.ToLowerInvariant(current));
            }
            return result.ToString();
        }

        private static void Add(StringBuilder line, Vector3 value)
        {
            Add(line, value.X);
            Add(line, value.Y);
            Add(line, value.Z);
        }

        private static void Add(StringBuilder line, bool value) =>
            AddRaw(line, value ? "1" : "0");

        private static void Add(StringBuilder line, float value) =>
            AddRaw(line, value.ToString("R", Invariant));

        private static void Add(StringBuilder line, int value) =>
            AddRaw(line, value.ToString(Invariant));

        private static void Add(StringBuilder line, uint value) =>
            AddRaw(line, value.ToString(Invariant));

        private static void Add(StringBuilder line, long value) =>
            AddRaw(line, value.ToString(Invariant));

        private static void Add(StringBuilder line, string? value)
        {
            value ??= string.Empty;
            if (line.Length != 0)
                line.Append(',');

            bool quote = value.IndexOfAny(new[] { ',', '"', '\r', '\n' }) >= 0;
            if (!quote)
            {
                line.Append(value);
                return;
            }

            line.Append('"');
            foreach (char character in value)
            {
                if (character == '"')
                    line.Append("\"\"");
                else
                    line.Append(character);
            }
            line.Append('"');
        }

        private static void AddRaw(StringBuilder line, string value)
        {
            if (line.Length != 0)
                line.Append(',');
            line.Append(value);
        }

        private static void AddEmpty(StringBuilder line, int count)
        {
            for (int i = 0; i < count; i++)
            {
                if (line.Length != 0)
                    line.Append(',');
            }
        }
    }

    /// <summary>
    /// Thread-safe streaming owner for one World3D calibration CSV. Start uses
    /// CreateNew so a repeated filename can never silently overwrite a take.
    /// Each appended row is flushed through StreamWriter immediately.
    /// </summary>
    public sealed class World3DCalibrationRecorder : IDisposable
    {
        private readonly object _gate = new();
        private StreamWriter? _writer;
        private long _startedTimestampMs;
        private string? _filePath;

        public bool IsRecording
        {
            get
            {
                lock (_gate)
                    return _writer != null;
            }
        }

        public string? FilePath
        {
            get
            {
                lock (_gate)
                    return _filePath;
            }
        }

        public void Start(string filePath)
        {
            if (string.IsNullOrWhiteSpace(filePath))
                throw new ArgumentException("A capture path is required.", nameof(filePath));

            lock (_gate)
            {
                if (_writer != null)
                    throw new InvalidOperationException("A World3D calibration capture is already active.");

                string fullPath = Path.GetFullPath(filePath);
                string? directory = Path.GetDirectoryName(fullPath);
                if (!string.IsNullOrEmpty(directory))
                    Directory.CreateDirectory(directory);

                FileStream? stream = null;
                StreamWriter? writer = null;
                try
                {
                    stream = new FileStream(
                        fullPath,
                        FileMode.CreateNew,
                        FileAccess.Write,
                        FileShare.Read,
                        64 * 1024,
                        FileOptions.SequentialScan);
                    writer = new StreamWriter(
                        stream,
                        new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                        64 * 1024,
                        leaveOpen: false)
                    {
                        NewLine = "\n",
                    };
                    stream = null;
                    World3DCalibrationCsvSerializer.WriteHeader(writer);
                    writer.Flush();
                    writer.AutoFlush = true;

                    _startedTimestampMs = Environment.TickCount64;
                    _filePath = fullPath;
                    _writer = writer;
                    writer = null;
                }
                finally
                {
                    writer?.Dispose();
                    stream?.Dispose();
                }
            }
        }

        public void Append(
            World3DCalibrationLabels labels,
            WorldLandmarkFrame landmarks,
            BodyTrackerFrame? convertedFrame,
            World3DLegCapturePair legs) =>
            Append(labels, landmarks, convertedFrame, legs.Left, legs.Right);

        public void Append(
            World3DCalibrationLabels labels,
            WorldLandmarkFrame landmarks,
            BodyTrackerFrame? convertedFrame,
            World3DLegCaptureFields leftLeg,
            World3DLegCaptureFields rightLeg)
        {
            ArgumentNullException.ThrowIfNull(landmarks);
            lock (_gate)
            {
                if (_writer == null)
                    throw new InvalidOperationException("Start must be called before Append.");

                long nowMs = Environment.TickCount64;
                var record = new World3DCalibrationRecord(
                    nowMs,
                    Math.Max(0L, nowMs - _startedTimestampMs),
                    DateTimeOffset.UtcNow,
                    labels,
                    landmarks,
                    convertedFrame,
                    leftLeg,
                    rightLeg);
                try
                {
                    World3DCalibrationCsvSerializer.WriteRow(_writer, record);
                }
                catch
                {
                    DisposeWriterNoThrow();
                    throw;
                }
            }
        }

        public void Stop()
        {
            lock (_gate)
                DisposeWriterNoThrow();
        }

        public void Dispose() => Stop();

        private void DisposeWriterNoThrow()
        {
            StreamWriter? writer = _writer;
            _writer = null;
            _filePath = null;
            _startedTimestampMs = 0;
            if (writer == null)
                return;

            try { writer.Flush(); }
            catch { }
            try { writer.Dispose(); }
            catch { }
        }
    }
}
