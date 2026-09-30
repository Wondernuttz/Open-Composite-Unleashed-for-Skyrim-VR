using System;
using System.Collections.Immutable;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>
    /// Stable landmark order produced by the MediaPipe Pose Landmarker model.
    /// </summary>
    public enum MediaPipePoseLandmarkIndex
    {
        Nose = 0,
        LeftEyeInner = 1,
        LeftEye = 2,
        LeftEyeOuter = 3,
        RightEyeInner = 4,
        RightEye = 5,
        RightEyeOuter = 6,
        LeftEar = 7,
        RightEar = 8,
        MouthLeft = 9,
        MouthRight = 10,
        LeftShoulder = 11,
        RightShoulder = 12,
        LeftElbow = 13,
        RightElbow = 14,
        LeftWrist = 15,
        RightWrist = 16,
        LeftPinky = 17,
        RightPinky = 18,
        LeftIndex = 19,
        RightIndex = 20,
        LeftThumb = 21,
        RightThumb = 22,
        LeftHip = 23,
        RightHip = 24,
        LeftKnee = 25,
        RightKnee = 26,
        LeftAnkle = 27,
        RightAnkle = 28,
        LeftHeel = 29,
        RightHeel = 30,
        LeftFootIndex = 31,
        RightFootIndex = 32,
    }

    /// <summary>
    /// One immutable MediaPipe landmark. Confidence is the lower of presence
    /// and visibility when both scores are supplied by the model.
    /// </summary>
    public readonly struct MediaPipePoseLandmark
    {
        internal MediaPipePoseLandmark(
            MediaPipePoseLandmarkIndex index,
            float x,
            float y,
            float z,
            bool hasVisibility,
            float visibility,
            bool hasPresence,
            float presence)
        {
            Index = index;
            X = x;
            Y = y;
            Z = z;
            HasVisibility = hasVisibility;
            Visibility = visibility;
            HasPresence = hasPresence;
            Presence = presence;
        }

        public MediaPipePoseLandmarkIndex Index { get; }
        public float X { get; }
        public float Y { get; }
        public float Z { get; }
        public bool HasVisibility { get; }
        public float Visibility { get; }
        public bool HasPresence { get; }
        public float Presence { get; }

        public float Confidence
        {
            get
            {
                if (HasVisibility && HasPresence)
                    return MathF.Min(Visibility, Presence);
                if (HasVisibility)
                    return Visibility;
                if (HasPresence)
                    return Presence;
                return 0.0f;
            }
        }
    }

    /// <summary>
    /// One detected pose. NormalizedLandmarks are image-relative; WorldLandmarks
    /// are in meters with an origin near the midpoint of the hips. World data is
    /// body-relative, not an absolute room-space position.
    /// </summary>
    public sealed class MediaPipePoseFrame
    {
        public const int LandmarkCount = 33;

        internal MediaPipePoseFrame(
            long timestampMilliseconds,
            ImmutableArray<MediaPipePoseLandmark> normalizedLandmarks,
            ImmutableArray<MediaPipePoseLandmark> worldLandmarks)
        {
            if (normalizedLandmarks.Length != LandmarkCount)
                throw new ArgumentException("A pose must contain exactly 33 normalized landmarks.", nameof(normalizedLandmarks));
            if (worldLandmarks.Length != LandmarkCount)
                throw new ArgumentException("A pose must contain exactly 33 world landmarks.", nameof(worldLandmarks));

            TimestampMilliseconds = timestampMilliseconds;
            NormalizedLandmarks = normalizedLandmarks;
            WorldLandmarks = worldLandmarks;

            float sum = 0.0f;
            float minimum = 1.0f;
            for (int i = 0; i < normalizedLandmarks.Length; i++)
            {
                float confidence = normalizedLandmarks[i].Confidence;
                sum += confidence;
                minimum = MathF.Min(minimum, confidence);
            }

            MeanConfidence = sum / LandmarkCount;
            MinimumConfidence = minimum;
        }

        public long TimestampMilliseconds { get; }
        public ImmutableArray<MediaPipePoseLandmark> NormalizedLandmarks { get; }
        public ImmutableArray<MediaPipePoseLandmark> WorldLandmarks { get; }
        public float MeanConfidence { get; }
        public float MinimumConfidence { get; }

        public MediaPipePoseLandmark GetNormalized(MediaPipePoseLandmarkIndex index) =>
            NormalizedLandmarks[(int)index];

        public MediaPipePoseLandmark GetWorld(MediaPipePoseLandmarkIndex index) =>
            WorldLandmarks[(int)index];
    }

    /// <summary>Native MediaPipe task failure, including its absl status code.</summary>
    public sealed class MediaPipePoseException : Exception
    {
        internal MediaPipePoseException(string operation, int statusCode, string? nativeMessage)
            : base(string.IsNullOrWhiteSpace(nativeMessage)
                ? operation + " failed with MediaPipe status " + statusCode + "."
                : operation + " failed with MediaPipe status " + statusCode + ": " + nativeMessage)
        {
            Operation = operation;
            StatusCode = statusCode;
            NativeMessage = nativeMessage;
        }

        public string Operation { get; }
        public int StatusCode { get; }
        public string? NativeMessage { get; }
    }
}
