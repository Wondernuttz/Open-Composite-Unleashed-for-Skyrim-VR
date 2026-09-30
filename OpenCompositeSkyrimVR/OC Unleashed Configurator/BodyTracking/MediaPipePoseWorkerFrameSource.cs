using System;
using System.Numerics;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>
    /// Adapts the native MediaPipePoseWorker API to the neutral immutable frame
    /// contract used by the converter and mux. It does not own or dispose the
    /// worker. A completed native frame with no pose becomes an explicit invalid
    /// frame, which is how pose loss triggers immediate mux fallback.
    /// </summary>
    public sealed class MediaPipePoseWorkerFrameSource : IWorldLandmarkFrameSource
    {
        private readonly object _gate = new object();
        private readonly MediaPipePoseWorker _worker;

        private uint _sourceEpoch;
        private int _imageWidth;
        private int _imageHeight;
        private bool _inputMirrored;
        private long _sequence;
        private long _adaptedCompletedTimestamp = -1;
        private WorldLandmarkFrame? _latest;

        public MediaPipePoseWorkerFrameSource(
            MediaPipePoseWorker worker,
            uint sourceEpoch,
            int imageWidth = 0,
            int imageHeight = 0,
            bool inputMirrored = false)
        {
            _worker = worker ?? throw new ArgumentNullException(nameof(worker));
            ValidateImageSize(imageWidth, imageHeight);
            _sourceEpoch = sourceEpoch;
            _imageWidth = imageWidth;
            _imageHeight = imageHeight;
            _inputMirrored = inputMirrored;
        }

        public uint SourceEpoch
        {
            get
            {
                lock (_gate)
                    return _sourceEpoch;
            }
        }

        public void BeginNewEpoch(uint sourceEpoch)
        {
            lock (_gate)
            {
                _sourceEpoch = sourceEpoch;
                _sequence = 0;
                _adaptedCompletedTimestamp = -1;
                _latest = null;
            }
        }

        public void UpdateInputMetadata(
            int imageWidth,
            int imageHeight,
            bool inputMirrored)
        {
            ValidateImageSize(imageWidth, imageHeight);
            lock (_gate)
            {
                _imageWidth = imageWidth;
                _imageHeight = imageHeight;
                _inputMirrored = inputMirrored;
            }
        }

        public bool TryGetLatestFrame(out WorldLandmarkFrame frame)
        {
            lock (_gate)
            {
                // Timestamp + result are published as one immutable worker
                // snapshot, so a newer pose can never be paired with an older
                // completion timestamp and interpreted as false pose loss.
                if (!_worker.TryGetLatestCompletion(
                    out long completed,
                    out MediaPipePoseFrame? native))
                {
                    frame = null!;
                    return false;
                }

                if (_latest == null || completed != _adaptedCompletedTimestamp)
                {
                    _sequence++;
                    _latest = native != null
                        && native.TimestampMilliseconds == completed
                        ? AdaptNativeFrame(
                            native,
                            _sequence,
                            _sourceEpoch,
                            _imageWidth,
                            _imageHeight,
                            _inputMirrored)
                        : CreateInvalidFrame(
                            _sequence,
                            completed,
                            _sourceEpoch,
                            _imageWidth,
                            _imageHeight,
                            _inputMirrored);
                    _adaptedCompletedTimestamp = completed;
                }

                frame = _latest;
                return true;
            }
        }

        public static WorldLandmarkFrame AdaptNativeFrame(
            MediaPipePoseFrame native,
            long sequence,
            uint sourceEpoch,
            int imageWidth,
            int imageHeight,
            bool inputMirrored)
        {
            ArgumentNullException.ThrowIfNull(native);
            ValidateImageSize(imageWidth, imageHeight);
            var landmarks = new WorldLandmark[WorldLandmarkFrame.MediaPipeLandmarkCount];
            for (int i = 0; i < landmarks.Length; i++)
            {
                MediaPipePoseLandmark normalized = native.NormalizedLandmarks[i];
                MediaPipePoseLandmark world = native.WorldLandmarks[i];
                float confidence = CombineConfidence(
                    normalized.Confidence,
                    world.Confidence);
                var normalizedPosition = new Vector3(
                    normalized.X, normalized.Y, normalized.Z);
                var worldPosition = new Vector3(world.X, world.Y, world.Z);
                bool valid = normalized.Index == (MediaPipePoseLandmarkIndex)i
                    && world.Index == (MediaPipePoseLandmarkIndex)i
                    && WorldLandmark.IsFinite(normalizedPosition)
                    && WorldLandmark.IsFinite(worldPosition);
                landmarks[i] = new WorldLandmark(
                    normalizedPosition,
                    worldPosition,
                    confidence,
                    valid);
            }

            return new WorldLandmarkFrame(
                sequence,
                native.TimestampMilliseconds,
                sourceEpoch,
                BodyPoseSourceFlags.NativeWorker,
                imageWidth,
                imageHeight,
                inputMirrored,
                landmarks);
        }

        private static WorldLandmarkFrame CreateInvalidFrame(
            long sequence,
            long timestampMs,
            uint sourceEpoch,
            int imageWidth,
            int imageHeight,
            bool inputMirrored)
        {
            var landmarks = new WorldLandmark[WorldLandmarkFrame.MediaPipeLandmarkCount];
            return new WorldLandmarkFrame(
                sequence,
                timestampMs,
                sourceEpoch,
                BodyPoseSourceFlags.NativeWorker,
                imageWidth,
                imageHeight,
                inputMirrored,
                landmarks);
        }

        private static float CombineConfidence(float normalized, float world)
        {
            normalized = float.IsFinite(normalized)
                ? Math.Clamp(normalized, 0f, 1f) : 0f;
            world = float.IsFinite(world)
                ? Math.Clamp(world, 0f, 1f) : 0f;
            if (normalized > 0f && world > 0f)
                return MathF.Min(normalized, world);
            return MathF.Max(normalized, world);
        }

        private static void ValidateImageSize(int width, int height)
        {
            if (width < 0)
                throw new ArgumentOutOfRangeException(nameof(width));
            if (height < 0)
                throw new ArgumentOutOfRangeException(nameof(height));
        }
    }
}
