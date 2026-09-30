using System;
using System.Collections.Generic;
using System.Numerics;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>
    /// Describes where a pose came from and which coordinate guarantees it
    /// carries. These flags describe the sender; they are deliberately separate
    /// from <see cref="OscTrackerFrameFlags"/>, which control runtime policy.
    /// </summary>
    [Flags]
    public enum BodyPoseSourceFlags : uint
    {
        None = 0,
        Legacy2DReconstruction = 1u << 0,
        Continuous3D = 1u << 1,
        MediaPipe33 = 1u << 2,
        MetricWorldCoordinates = 1u << 3,
        NormalizedImageCoordinates = 1u << 4,
        LandmarkConfidence = 1u << 5,
        NativeWorker = 1u << 6,
        MirroredInput = 1u << 7,
    }

    public enum BodyTrackerSource : byte
    {
        None = 0,
        Legacy2D = 1,
        Continuous3D = 2,
    }

    /// <summary>
    /// One model landmark. WorldPosition is the model's hip-centred 3D output
    /// in metres. NormalizedPosition is image x/y plus model-relative z.
    /// Confidence should already combine the worker's visibility/presence
    /// signals into [0,1].
    /// </summary>
    public readonly struct WorldLandmark
    {
        public WorldLandmark(
            Vector3 normalizedPosition,
            Vector3 worldPosition,
            float confidence,
            bool isValid = true)
        {
            NormalizedPosition = normalizedPosition;
            WorldPosition = worldPosition;
            Confidence = float.IsFinite(confidence)
                ? Math.Clamp(confidence, 0f, 1f) : 0f;
            IsValid = isValid
                && IsFinite(normalizedPosition)
                && IsFinite(worldPosition);
        }

        public Vector3 NormalizedPosition { get; }
        public Vector3 WorldPosition { get; }
        public float Confidence { get; }
        public bool IsValid { get; }

        public bool IsWorldUsable(float minimumConfidence) =>
            IsValid && Confidence >= minimumConfidence && IsFinite(WorldPosition);

        internal static bool IsFinite(Vector3 value) =>
            float.IsFinite(value.X)
            && float.IsFinite(value.Y)
            && float.IsFinite(value.Z);
    }

    /// <summary>
    /// Immutable worker-to-configurator pose snapshot. The constructor clones
    /// all 33 landmarks, so a native worker may immediately reuse its buffers.
    /// TimestampMs must use the same monotonic millisecond clock supplied to the
    /// mux. SourceEpoch changes whenever the worker/calibration restarts.
    /// </summary>
    public sealed class WorldLandmarkFrame
    {
        public const int MediaPipeLandmarkCount = 33;

        private readonly WorldLandmark[] _landmarks;

        public WorldLandmarkFrame(
            long sequence,
            long timestampMs,
            uint sourceEpoch,
            BodyPoseSourceFlags sourceFlags,
            int imageWidth,
            int imageHeight,
            bool inputMirrored,
            IReadOnlyList<WorldLandmark> landmarks)
        {
            ArgumentNullException.ThrowIfNull(landmarks);
            if (landmarks.Count != MediaPipeLandmarkCount)
                throw new ArgumentException(
                    $"A MediaPipe pose must contain exactly {MediaPipeLandmarkCount} landmarks.",
                    nameof(landmarks));
            if (imageWidth < 0)
                throw new ArgumentOutOfRangeException(nameof(imageWidth));
            if (imageHeight < 0)
                throw new ArgumentOutOfRangeException(nameof(imageHeight));

            Sequence = sequence;
            TimestampMs = timestampMs;
            SourceEpoch = sourceEpoch;
            ImageWidth = imageWidth;
            ImageHeight = imageHeight;
            InputMirrored = inputMirrored;
            SourceFlags = sourceFlags
                | BodyPoseSourceFlags.Continuous3D
                | BodyPoseSourceFlags.MediaPipe33
                | BodyPoseSourceFlags.MetricWorldCoordinates
                | BodyPoseSourceFlags.NormalizedImageCoordinates
                | BodyPoseSourceFlags.LandmarkConfidence
                | (inputMirrored ? BodyPoseSourceFlags.MirroredInput : 0);

            _landmarks = new WorldLandmark[MediaPipeLandmarkCount];
            for (int i = 0; i < _landmarks.Length; i++)
                _landmarks[i] = landmarks[i];
        }

        public long Sequence { get; }
        public long TimestampMs { get; }
        public uint SourceEpoch { get; }
        public BodyPoseSourceFlags SourceFlags { get; }
        public int ImageWidth { get; }
        public int ImageHeight { get; }
        public bool InputMirrored { get; }
        public int LandmarkCount => _landmarks.Length;

        public WorldLandmark this[int index] => _landmarks[index];

        public WorldLandmark[] CopyLandmarks() =>
            (WorldLandmark[])_landmarks.Clone();
    }

    /// <summary>
    /// Integration boundary for an in-process or native pose worker. Returning
    /// false means no frame has ever been produced; returning a newer frame with
    /// invalid landmarks lets the mux fall back immediately instead of waiting
    /// for its stale timeout.
    /// </summary>
    public interface IWorldLandmarkFrameSource
    {
        bool TryGetLatestFrame(out WorldLandmarkFrame frame);
    }

    /// <summary>One OSC-compatible tracker pose in sender/Unity space.</summary>
    public readonly struct BodyTrackerPose
    {
        public BodyTrackerPose(
            Vector3 position,
            Quaternion orientation,
            float confidence,
            bool positionValid,
            bool rotationValid)
        {
            bool finitePosition = WorldLandmark.IsFinite(position);
            bool finiteRotation = IsFinite(orientation)
                && orientation.LengthSquared() > 1e-12f;

            PositionValid = positionValid && finitePosition;
            RotationValid = rotationValid && finiteRotation;
            Position = PositionValid ? position : Vector3.Zero;
            Orientation = RotationValid
                ? Quaternion.Normalize(orientation) : Quaternion.Identity;
            Confidence = float.IsFinite(confidence)
                ? Math.Clamp(confidence, 0f, 1f) : 0f;
        }

        public Vector3 Position { get; }
        public Quaternion Orientation { get; }
        public float Confidence { get; }
        public bool PositionValid { get; }
        public bool RotationValid { get; }
        public bool IsFullyValid => PositionValid && RotationValid;

        public static BodyTrackerPose Invalid => default;

        internal static bool IsFinite(Quaternion value) =>
            float.IsFinite(value.X)
            && float.IsFinite(value.Y)
            && float.IsFinite(value.Z)
            && float.IsFinite(value.W);
    }

    /// <summary>
    /// Immutable head plus OSC tracker slots 1..8. OCU uses slot 1 for waist,
    /// 2 for left foot, 3 for right foot, 4/5 for knees, 6/7 for elbows and 8
    /// for chest. A frame is a snapshot: callers cannot mutate its backing data.
    /// </summary>
    public sealed class BodyTrackerFrame
    {
        public const int TrackerSlotCount = 8;

        private readonly BodyTrackerPose[] _trackers;

        public BodyTrackerFrame(
            long sequence,
            long timestampMs,
            uint sourceEpoch,
            BodyTrackerSource source,
            BodyPoseSourceFlags sourceFlags,
            BodyTrackerPose head,
            IReadOnlyList<BodyTrackerPose> trackers)
        {
            ArgumentNullException.ThrowIfNull(trackers);
            if (trackers.Count != TrackerSlotCount)
                throw new ArgumentException(
                    $"A tracker frame must contain exactly {TrackerSlotCount} OSC slots.",
                    nameof(trackers));

            Sequence = sequence;
            TimestampMs = timestampMs;
            SourceEpoch = sourceEpoch;
            Source = source;
            SourceFlags = sourceFlags;
            Head = head;
            _trackers = new BodyTrackerPose[TrackerSlotCount];
            for (int i = 0; i < _trackers.Length; i++)
                _trackers[i] = trackers[i];
        }

        public long Sequence { get; }
        public long TimestampMs { get; }
        public uint SourceEpoch { get; }
        public BodyTrackerSource Source { get; }
        public BodyPoseSourceFlags SourceFlags { get; }
        public BodyTrackerPose Head { get; }

        /// <summary>True when head, waist and both feet have complete 6DoF poses.</summary>
        public bool CorePoseValid =>
            Head.IsFullyValid
            && GetTracker(1).IsFullyValid
            && GetTracker(2).IsFullyValid
            && GetTracker(3).IsFullyValid;

        public BodyTrackerPose GetTracker(int oscSlot)
        {
            if (oscSlot < 1 || oscSlot > TrackerSlotCount)
                throw new ArgumentOutOfRangeException(nameof(oscSlot));
            return _trackers[oscSlot - 1];
        }

        public BodyTrackerPose[] CopyTrackers() =>
            (BodyTrackerPose[])_trackers.Clone();

        public uint OscPoseMask => OscTrackerFrameContract.BuildPoseMask(this);
    }

    /// <summary>
    /// Policy bits consumed by OpenOVR's /tracking/trackers/frame receiver.
    /// Values must stay identical to NetTrackerFrameFlagBits in
    /// OpenOVR/Misc/NetworkTrackers.h.
    /// </summary>
    [Flags]
    public enum OscTrackerFrameFlags : uint
    {
        None = 0,
        UseHeadTranslation = 1u << 0,
        UseHeadHeightScale = 1u << 1,
        UseHeadYaw = 1u << 2,
        FollowHmdYaw = 1u << 3,
        AllowGaitFootRelease = 1u << 4,
        Continuous3D = 1u << 5,
        // The sender's synthetic head has the same horizontal origin as the
        // body/hip root. The runtime may therefore hard-anchor X/Z to the real
        // HMD without turning torso-model jitter into waist drift.
        HeadXZIsBodyRoot = 1u << 6,
    }

    public static class OscTrackerFrameContract
    {
        public const string Address = "/tracking/trackers/frame";
        public const uint TrackerPositionMask = 0x000000ffu;
        public const uint TrackerRotationMask = 0x0000ff00u;
        public const uint HeadPositionBit = 1u << 16;
        public const uint HeadRotationBit = 1u << 17;
        public const uint ValidPoseMask = (1u << 18) - 1u;
        public const uint MaximumExactFloatInteger = 0x00ffffffu;

        public static uint TrackerPositionBit(int oscSlot)
        {
            ValidateSlot(oscSlot);
            return 1u << (oscSlot - 1);
        }

        public static uint TrackerRotationBit(int oscSlot)
        {
            ValidateSlot(oscSlot);
            return 1u << (oscSlot + 7);
        }

        public static uint BuildPoseMask(BodyTrackerFrame frame)
        {
            ArgumentNullException.ThrowIfNull(frame);
            uint mask = 0;
            for (int slot = 1; slot <= BodyTrackerFrame.TrackerSlotCount; slot++)
            {
                BodyTrackerPose pose = frame.GetTracker(slot);
                if (pose.PositionValid)
                    mask |= TrackerPositionBit(slot);
                if (pose.RotationValid)
                    mask |= TrackerRotationBit(slot);
            }
            if (frame.Head.PositionValid)
                mask |= HeadPositionBit;
            if (frame.Head.RotationValid)
                mask |= HeadRotationBit;
            return mask;
        }

        public static OscTrackerFrameFlags BuildPolicyFlags(
            BodyTrackerFrame frame,
            bool followHmdYaw,
            bool allowGaitFootRelease)
        {
            ArgumentNullException.ThrowIfNull(frame);
            OscTrackerFrameFlags flags = OscTrackerFrameFlags.None;
            if (frame.Head.PositionValid)
            {
                flags |= OscTrackerFrameFlags.UseHeadTranslation;
                flags |= OscTrackerFrameFlags.UseHeadHeightScale;
            }
            if (frame.Head.RotationValid)
                flags |= OscTrackerFrameFlags.UseHeadYaw;
            if (followHmdYaw)
                flags |= OscTrackerFrameFlags.FollowHmdYaw;
            if (allowGaitFootRelease)
                flags |= OscTrackerFrameFlags.AllowGaitFootRelease;
            if ((frame.SourceFlags & BodyPoseSourceFlags.Continuous3D) != 0)
            {
                flags |= OscTrackerFrameFlags.Continuous3D;
                if ((frame.SourceFlags & BodyPoseSourceFlags.MediaPipe33) != 0)
                    flags |= OscTrackerFrameFlags.HeadXZIsBodyRoot;
            }
            return flags;
        }

        /// <summary>
        /// Returns the three exactly-representable integer floats sent as the
        /// final OSC message in a tracker snapshot: epoch, pose mask, flags.
        /// </summary>
        public static Vector3 PackHeader(
            uint sourceEpoch,
            uint poseMask,
            OscTrackerFrameFlags flags)
        {
            if (sourceEpoch > MaximumExactFloatInteger)
                throw new ArgumentOutOfRangeException(nameof(sourceEpoch));
            if ((poseMask & ~ValidPoseMask) != 0)
                throw new ArgumentOutOfRangeException(nameof(poseMask));
            uint rawFlags = (uint)flags;
            if (rawFlags > MaximumExactFloatInteger)
                throw new ArgumentOutOfRangeException(nameof(flags));
            return new Vector3(sourceEpoch, poseMask, rawFlags);
        }

        private static void ValidateSlot(int oscSlot)
        {
            if (oscSlot < 1 || oscSlot > BodyTrackerFrame.TrackerSlotCount)
                throw new ArgumentOutOfRangeException(nameof(oscSlot));
        }
    }
}
