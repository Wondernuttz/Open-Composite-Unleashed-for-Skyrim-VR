using System;
using System.Buffers;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>
    /// Single-consumer pose worker with a one-frame input queue. If capture is
    /// faster than inference, the queued stale frame is returned to the pool and
    /// replaced by the newest frame.
    /// </summary>
    public sealed class MediaPipePoseWorker : IDisposable
    {
        private readonly object _submitGate = new object();
        private readonly MediaPipePoseLandmarker _landmarker;
        private readonly bool _ownsLandmarker;
        private readonly Channel<PendingFrame> _input;
        private readonly Channel<MediaPipePoseFrame> _output;
        private readonly CancellationTokenSource _shutdown = new CancellationTokenSource();
        private readonly Task _processingTask;

        private MediaPipePoseFrame? _latestFrame;
        private CompletionSnapshot? _latestCompletion;
        private Exception? _lastError;
        private long _lastSubmittedTimestampMilliseconds = -1;
        private long _lastCompletedTimestampMilliseconds = -1;
        private long _droppedFrameCount;
        private int _disposed;

        public MediaPipePoseWorker(MediaPipePoseLandmarkerOptions? options = null)
            : this(new MediaPipePoseLandmarker(options), true)
        {
        }

        public MediaPipePoseWorker(MediaPipePoseLandmarker landmarker, bool ownsLandmarker = true)
        {
            _landmarker = landmarker ?? throw new ArgumentNullException(nameof(landmarker));
            _ownsLandmarker = ownsLandmarker;

            BoundedChannelOptions inputOptions = new BoundedChannelOptions(1)
            {
                AllowSynchronousContinuations = false,
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
                SingleWriter = false,
            };
            _input = Channel.CreateBounded<PendingFrame>(inputOptions, OnInputDropped);

            BoundedChannelOptions outputOptions = new BoundedChannelOptions(1)
            {
                AllowSynchronousContinuations = false,
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = false,
                SingleWriter = true,
            };
            _output = Channel.CreateBounded<MediaPipePoseFrame>(outputOptions);
            _processingTask = Task.Run(ProcessFramesAsync);
        }

        /// <summary>
        /// Pose detections only. A processed frame with no pose clears
        /// LatestFrame; compare LastCompletedTimestampMilliseconds with the
        /// latest detection timestamp when pose-loss state matters.
        /// </summary>
        public ChannelReader<MediaPipePoseFrame> Frames => _output.Reader;

        public Task Completion => _processingTask;
        public MediaPipePoseFrame? LatestFrame => Volatile.Read(ref _latestFrame);
        public Exception? LastError => Volatile.Read(ref _lastError);
        public long LastSubmittedTimestampMilliseconds =>
            Interlocked.Read(ref _lastSubmittedTimestampMilliseconds);
        public long LastCompletedTimestampMilliseconds =>
            Interlocked.Read(ref _lastCompletedTimestampMilliseconds);
        public long DroppedFrameCount => Interlocked.Read(ref _droppedFrameCount);

        /// <summary>
        /// Reads the completed timestamp and its pose as one immutable publish.
        /// The pose is null when inference completed without a valid body or
        /// failed for that frame.
        /// </summary>
        public bool TryGetLatestCompletion(
            out long timestampMilliseconds,
            out MediaPipePoseFrame? frame)
        {
            CompletionSnapshot? snapshot = Volatile.Read(ref _latestCompletion);
            if (snapshot == null)
            {
                timestampMilliseconds = -1;
                frame = null;
                return false;
            }
            timestampMilliseconds = snapshot.TimestampMilliseconds;
            frame = snapshot.Frame;
            return true;
        }

        /// <summary>
        /// Copies one packed RGB24 frame into pool-owned memory. The caller can
        /// immediately reuse its capture buffer after this method returns.
        /// </summary>
        public bool TrySubmitRgb24(
            ReadOnlySpan<byte> rgb24,
            int width,
            int height,
            long timestampMilliseconds)
        {
            int packedStride = checked(width * MediaPipePoseLandmarker.RgbChannelCount);
            return TrySubmitRgb24(rgb24, width, height, packedStride, timestampMilliseconds);
        }

        /// <summary>
        /// Copies an RGB24 frame whose rows may contain padding. This is suitable
        /// for camera/OpenCV buffers after BGR-to-RGB conversion.
        /// </summary>
        public bool TrySubmitRgb24(
            ReadOnlySpan<byte> rgb24,
            int width,
            int height,
            int stride,
            long timestampMilliseconds)
        {
            if (Volatile.Read(ref _disposed) != 0)
                return false;

            int packedLength = MediaPipePoseLandmarker.GetPackedRgbLength(width, height);
            int packedStride = checked(width * MediaPipePoseLandmarker.RgbChannelCount);
            ValidateSubmission(timestampMilliseconds, stride, packedStride, height);

            long requiredSourceLength = checked(((long)height - 1L) * stride + packedStride);
            if (rgb24.Length < requiredSourceLength)
                throw new ArgumentException("The RGB source buffer is too small for its stride.", nameof(rgb24));

            byte[] buffer = ArrayPool<byte>.Shared.Rent(packedLength);
            PendingFrame? pending = null;
            try
            {
                for (int row = 0; row < height; row++)
                {
                    rgb24.Slice(row * stride, packedStride).CopyTo(
                        buffer.AsSpan(row * packedStride, packedStride));
                }

                pending = new PendingFrame(buffer, packedLength, width, height, timestampMilliseconds);
                buffer = null!;
                return TryQueue(pending);
            }
            finally
            {
                if (buffer != null)
                    ArrayPool<byte>.Shared.Return(buffer);
            }
        }

        /// <summary>
        /// Copies an unmanaged, optionally padded RGB24 frame. The pointer only
        /// needs to remain valid until this method returns.
        /// </summary>
        public bool TrySubmitRgb24(
            IntPtr rgb24,
            int width,
            int height,
            int stride,
            long timestampMilliseconds)
        {
            if (Volatile.Read(ref _disposed) != 0)
                return false;
            if (rgb24 == IntPtr.Zero)
                throw new ArgumentNullException(nameof(rgb24));

            int packedLength = MediaPipePoseLandmarker.GetPackedRgbLength(width, height);
            int packedStride = checked(width * MediaPipePoseLandmarker.RgbChannelCount);
            ValidateSubmission(timestampMilliseconds, stride, packedStride, height);

            byte[] buffer = ArrayPool<byte>.Shared.Rent(packedLength);
            PendingFrame? pending = null;
            try
            {
                for (int row = 0; row < height; row++)
                {
                    int sourceOffset = checked(row * stride);
                    Marshal.Copy(
                        IntPtr.Add(rgb24, sourceOffset),
                        buffer,
                        row * packedStride,
                        packedStride);
                }

                pending = new PendingFrame(buffer, packedLength, width, height, timestampMilliseconds);
                buffer = null!;
                return TryQueue(pending);
            }
            finally
            {
                if (buffer != null)
                    ArrayPool<byte>.Shared.Return(buffer);
            }
        }

        public void Dispose()
        {
            lock (_submitGate)
            {
                if (Interlocked.Exchange(ref _disposed, 1) != 0)
                    return;
                _input.Writer.TryComplete();
            }

            _shutdown.Cancel();
            try
            {
                _processingTask.GetAwaiter().GetResult();
            }
            finally
            {
                _shutdown.Dispose();
                if (_ownsLandmarker)
                    _landmarker.Dispose();
            }

            GC.SuppressFinalize(this);
        }

        private bool TryQueue(PendingFrame pending)
        {
            lock (_submitGate)
            {
                if (Volatile.Read(ref _disposed) != 0)
                {
                    pending.Return();
                    return false;
                }
                if (pending.TimestampMilliseconds <= _lastSubmittedTimestampMilliseconds)
                {
                    pending.Return();
                    throw new ArgumentOutOfRangeException(
                        nameof(pending.TimestampMilliseconds),
                        "Submitted timestamps must be strictly increasing.");
                }
                if (!_input.Writer.TryWrite(pending))
                {
                    pending.Return();
                    return false;
                }

                Interlocked.Exchange(
                    ref _lastSubmittedTimestampMilliseconds,
                    pending.TimestampMilliseconds);
                return true;
            }
        }

        private async Task ProcessFramesAsync()
        {
            Exception? terminalError = null;
            try
            {
                while (await _input.Reader.WaitToReadAsync(_shutdown.Token).ConfigureAwait(false))
                {
                    while (_input.Reader.TryRead(out PendingFrame? pending))
                    {
                        try
                        {
                            MediaPipePoseFrame? pose = _landmarker.DetectForVideo(
                                pending.Buffer,
                                pending.Width,
                                pending.Height,
                                pending.TimestampMilliseconds);

                            Volatile.Write(ref _latestCompletion,
                                new CompletionSnapshot(pending.TimestampMilliseconds, pose));
                            Volatile.Write(ref _latestFrame, pose);
                            Volatile.Write(ref _lastError, null);
                            Interlocked.Exchange(
                                ref _lastCompletedTimestampMilliseconds,
                                pending.TimestampMilliseconds);
                            if (pose != null)
                                _output.Writer.TryWrite(pose);
                        }
                        catch (Exception exception) when (exception is not OperationCanceledException)
                        {
                            Volatile.Write(ref _latestCompletion,
                                new CompletionSnapshot(pending.TimestampMilliseconds, null));
                            Volatile.Write(ref _latestFrame, null);
                            Volatile.Write(ref _lastError, exception);
                            Interlocked.Exchange(
                                ref _lastCompletedTimestampMilliseconds,
                                pending.TimestampMilliseconds);
                        }
                        finally
                        {
                            pending.Return();
                        }

                        if (_shutdown.IsCancellationRequested)
                            break;
                    }
                }
            }
            catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
            {
                // Normal disposal path.
            }
            catch (Exception exception)
            {
                terminalError = exception;
                Volatile.Write(ref _latestFrame, null);
                Volatile.Write(ref _lastError, exception);
            }
            finally
            {
                while (_input.Reader.TryRead(out PendingFrame? pending))
                    pending.Return();
                _output.Writer.TryComplete(terminalError);
            }
        }

        private void OnInputDropped(PendingFrame pending)
        {
            Interlocked.Increment(ref _droppedFrameCount);
            pending.Return();
        }

        private static void ValidateSubmission(
            long timestampMilliseconds,
            int stride,
            int packedStride,
            int height)
        {
            if (timestampMilliseconds < 0)
                throw new ArgumentOutOfRangeException(nameof(timestampMilliseconds));
            if (stride < packedStride)
                throw new ArgumentOutOfRangeException(nameof(stride), "Stride is smaller than one RGB row.");
            if (((long)height - 1L) * stride > int.MaxValue)
                throw new ArgumentOutOfRangeException(nameof(stride), "The source stride is too large.");
        }

        private sealed class PendingFrame
        {
            private byte[]? _buffer;

            public PendingFrame(
                byte[] buffer,
                int dataLength,
                int width,
                int height,
                long timestampMilliseconds)
            {
                _buffer = buffer;
                DataLength = dataLength;
                Width = width;
                Height = height;
                TimestampMilliseconds = timestampMilliseconds;
            }

            public byte[] Buffer =>
                _buffer ?? throw new ObjectDisposedException(nameof(PendingFrame));
            public int DataLength { get; }
            public int Width { get; }
            public int Height { get; }
            public long TimestampMilliseconds { get; }

            public void Return()
            {
                byte[]? buffer = Interlocked.Exchange(ref _buffer, null);
                if (buffer != null)
                    ArrayPool<byte>.Shared.Return(buffer);
            }
        }

        private sealed class CompletionSnapshot
        {
            public CompletionSnapshot(
                long timestampMilliseconds,
                MediaPipePoseFrame? frame)
            {
                TimestampMilliseconds = timestampMilliseconds;
                Frame = frame;
            }

            public long TimestampMilliseconds { get; }
            public MediaPipePoseFrame? Frame { get; }
        }
    }
}
