using System;
using System.Collections.Immutable;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;

namespace OpenCompositeConfigurator.BodyTracking
{
    public sealed class MediaPipePoseLandmarkerOptions
    {
        public string? ModelPath { get; set; }
        public float MinimumPoseDetectionConfidence { get; set; } = 0.5f;
        public float MinimumPosePresenceConfidence { get; set; } = 0.5f;
        public float MinimumTrackingConfidence { get; set; } = 0.5f;
    }

    /// <summary>
    /// Synchronous, thread-safe wrapper over the MediaPipe v0.10.35 Pose
    /// Landmarker C API. The native DLL and model are hash-pinned because the C
    /// ABI is not stable between MediaPipe releases.
    /// </summary>
    public sealed class MediaPipePoseLandmarker : IDisposable
    {
        public const string BackendVersion = "0.10.35";
        public const string NativeLibrarySha256 = "8B272582856968B7C2B28B082ABE245799D3EE9243722D13A08DBCBEDBE7D0AD";
        public const string OpenCvNativeLibrarySha256 = "B925E1955AF1718ED1AE10D1F3F1DB57E5AB8C4865729E4748D0BA721EDCA687";
        public const string PoseModelSha256 = "4EAA5EB7A98365221087693FCC286334CF0858E2EB6E15B506AA4A7ECDCEC4AD";
        // pose_landmarker_lite float16 from the official MediaPipe model zoo.
        // Roughly 3-5x faster than full on CPU; full-model CPU inference was
        // measured at ~7 fps live, which is below what the leg classifier and
        // foot filters can tolerate.
        public const string PoseLiteModelSha256 = "59929E1D1EE95287735DDD833B19CF4AC46D29BC7AFDDBBF6753C459690D574A";
        public const int RgbChannelCount = 3;

        private const int NativeRgbFormat = 1;
        private const int NativeVideoRunningMode = 2;
        private const int NativeCpuDelegate = 0;
        private const int NativeUnknownEnvironment = 0;
        private const int NativeWindowsSystem = 3;

        private readonly object _gate = new object();
        private readonly NativeApi _api;
        private readonly PoseLandmarkerSafeHandle _landmarker;
        private bool _disposed;
        private long _lastTimestampMilliseconds = -1;

        public MediaPipePoseLandmarker(MediaPipePoseLandmarkerOptions? options = null)
        {
            options ??= new MediaPipePoseLandmarkerOptions();
            ValidateConfidence(options.MinimumPoseDetectionConfidence, nameof(options.MinimumPoseDetectionConfidence));
            ValidateConfidence(options.MinimumPosePresenceConfidence, nameof(options.MinimumPosePresenceConfidence));
            ValidateConfidence(options.MinimumTrackingConfidence, nameof(options.MinimumTrackingConfidence));

            string modelPath = Path.GetFullPath(options.ModelPath ?? DefaultModelPath);
            VerifyPinnedFile(modelPath, ExpectedModelSha256(modelPath), "MediaPipe pose model");

            _api = NativeApi.Instance;
            IntPtr modelPathUtf8 = Marshal.StringToCoTaskMemUTF8(modelPath);
            IntPtr rawLandmarker = IntPtr.Zero;
            try
            {
                NativePoseLandmarkerOptions nativeOptions = new NativePoseLandmarkerOptions
                {
                    BaseOptions = new NativeBaseOptions
                    {
                        ModelAssetPath = modelPathUtf8,
                        Delegate = NativeCpuDelegate,
                        HostEnvironment = NativeUnknownEnvironment,
                        HostSystem = NativeWindowsSystem,
                    },
                    RunningMode = NativeVideoRunningMode,
                    NumPoses = 1,
                    MinimumPoseDetectionConfidence = options.MinimumPoseDetectionConfidence,
                    MinimumPosePresenceConfidence = options.MinimumPosePresenceConfidence,
                    MinimumTrackingConfidence = options.MinimumTrackingConfidence,
                    OutputSegmentationMasks = 0,
                    ResultCallback = IntPtr.Zero,
                };

                IntPtr errorMessage = IntPtr.Zero;
                int status = _api.CreatePoseLandmarker(ref nativeOptions, ref rawLandmarker, ref errorMessage);
                _api.ThrowIfFailed(status, errorMessage, "MpPoseLandmarkerCreate");
                if (rawLandmarker == IntPtr.Zero)
                    throw new InvalidOperationException("MediaPipe returned success without a pose landmarker handle.");

                _landmarker = new PoseLandmarkerSafeHandle(_api, rawLandmarker);
                rawLandmarker = IntPtr.Zero;
                ModelPath = modelPath;
            }
            finally
            {
                Marshal.FreeCoTaskMem(modelPathUtf8);
                if (rawLandmarker != IntPtr.Zero)
                    _api.CloseOrphanedPoseLandmarker(rawLandmarker);
            }
        }

        public static string DefaultNativeLibraryPath =>
            Path.Combine(AppContext.BaseDirectory, "libmediapipe.dll");

        public static string DefaultOpenCvNativeLibraryPath =>
            Path.Combine(AppContext.BaseDirectory, "opencv_world3410.dll");

        public static string DefaultModelPath
        {
            get
            {
                string modelDirectory = Path.Combine(
                    AppContext.BaseDirectory,
                    "BodyTracking",
                    "MediaPipe",
                    "v0.10.35");
                string lite = Path.Combine(modelDirectory, "pose_landmarker_lite.task");
                return File.Exists(lite)
                    ? lite
                    : Path.Combine(modelDirectory, "pose_landmarker_full.task");
            }
        }

        private static string ExpectedModelSha256(string modelPath) =>
            Path.GetFileName(modelPath).Contains("lite", StringComparison.OrdinalIgnoreCase)
                ? PoseLiteModelSha256
                : PoseModelSha256;

        public string ModelPath { get; }

        public long LastTimestampMilliseconds
        {
            get
            {
                lock (_gate)
                    return _lastTimestampMilliseconds;
            }
        }

        /// <summary>Verifies and loads the exact pinned DLL and verifies the model.</summary>
        public static void VerifyPinnedAssets(string? modelPath = null)
        {
            string resolvedModelPath = Path.GetFullPath(modelPath ?? DefaultModelPath);
            VerifyPinnedFile(
                resolvedModelPath,
                ExpectedModelSha256(resolvedModelPath),
                "MediaPipe pose model");
            _ = NativeApi.Instance;
        }

        /// <summary>
        /// Runs one packed RGB24 frame. The native image constructor copies the
        /// pixels before this method returns.
        /// </summary>
        public MediaPipePoseFrame? DetectForVideo(
            byte[] rgb24,
            int width,
            int height,
            long timestampMilliseconds)
        {
            ArgumentNullException.ThrowIfNull(rgb24);
            int requiredLength = GetPackedRgbLength(width, height);
            if (rgb24.Length < requiredLength)
            {
                throw new ArgumentException(
                    "The RGB buffer is smaller than width * height * 3.",
                    nameof(rgb24));
            }

            GCHandle pinnedBuffer = GCHandle.Alloc(rgb24, GCHandleType.Pinned);
            try
            {
                return DetectForVideo(
                    pinnedBuffer.AddrOfPinnedObject(),
                    requiredLength,
                    width,
                    height,
                    timestampMilliseconds);
            }
            finally
            {
                pinnedBuffer.Free();
            }
        }

        /// <summary>
        /// Runs one packed RGB24 frame from unmanaged memory. The pointer only
        /// needs to remain valid for this call because MediaPipe copies it.
        /// </summary>
        public MediaPipePoseFrame? DetectForVideo(
            IntPtr rgb24,
            int byteLength,
            int width,
            int height,
            long timestampMilliseconds)
        {
            if (rgb24 == IntPtr.Zero)
                throw new ArgumentNullException(nameof(rgb24));
            int requiredLength = GetPackedRgbLength(width, height);
            if (byteLength < requiredLength)
                throw new ArgumentOutOfRangeException(nameof(byteLength), "The RGB buffer is too small.");
            if (timestampMilliseconds < 0)
                throw new ArgumentOutOfRangeException(nameof(timestampMilliseconds));

            lock (_gate)
            {
                ThrowIfDisposed();
                if (timestampMilliseconds <= _lastTimestampMilliseconds)
                {
                    throw new ArgumentOutOfRangeException(
                        nameof(timestampMilliseconds),
                        "Video timestamps must be strictly increasing.");
                }

                using MpImageSafeHandle image = CreateRgbImage(rgb24, requiredLength, width, height);

                NativePoseLandmarkerResult result = default;
                bool nativeCallReturned = false;
                _lastTimestampMilliseconds = timestampMilliseconds;
                try
                {
                    IntPtr errorMessage = IntPtr.Zero;
                    int status = _api.DetectForVideo(
                        _landmarker.DangerousGetHandle(),
                        image.DangerousGetHandle(),
                        IntPtr.Zero,
                        timestampMilliseconds,
                        ref result,
                        ref errorMessage);
                    nativeCallReturned = true;
                    _api.ThrowIfFailed(status, errorMessage, "MpPoseLandmarkerDetectForVideo");
                    return ConvertResult(result, timestampMilliseconds);
                }
                finally
                {
                    if (nativeCallReturned)
                        _api.ClosePoseLandmarkerResult(ref result);
                }
            }
        }

        public void Dispose()
        {
            lock (_gate)
            {
                if (_disposed)
                    return;

                _disposed = true;
                _landmarker.Dispose();
            }

            GC.SuppressFinalize(this);
        }

        internal static int GetPackedRgbLength(int width, int height)
        {
            if (width <= 0)
                throw new ArgumentOutOfRangeException(nameof(width));
            if (height <= 0)
                throw new ArgumentOutOfRangeException(nameof(height));

            try
            {
                return checked(width * height * RgbChannelCount);
            }
            catch (OverflowException)
            {
                throw new ArgumentOutOfRangeException(nameof(width), "The image dimensions are too large.");
            }
        }

        private MpImageSafeHandle CreateRgbImage(IntPtr pixels, int byteLength, int width, int height)
        {
            IntPtr rawImage = IntPtr.Zero;
            IntPtr errorMessage = IntPtr.Zero;
            int status = _api.CreateImageFromUint8Data(
                NativeRgbFormat,
                width,
                height,
                pixels,
                byteLength,
                ref rawImage,
                ref errorMessage);

            if (status != 0)
            {
                if (rawImage != IntPtr.Zero)
                    _api.FreeImage(rawImage);
                _api.ThrowIfFailed(status, errorMessage, "MpImageCreateFromUint8Data");
            }
            else
            {
                _api.FreeError(errorMessage);
            }

            if (rawImage == IntPtr.Zero)
                throw new InvalidOperationException("MediaPipe returned success without an image handle.");
            return new MpImageSafeHandle(_api, rawImage);
        }

        private static MediaPipePoseFrame? ConvertResult(
            NativePoseLandmarkerResult result,
            long timestampMilliseconds)
        {
            if (result.PoseLandmarksCount == 0 && result.PoseWorldLandmarksCount == 0)
                return null;

            if (result.PoseLandmarksCount != 1 || result.PoseWorldLandmarksCount != 1)
            {
                throw new InvalidDataException(
                    "The pinned single-pose graph returned mismatched or multiple poses.");
            }
            if (result.PoseLandmarks == IntPtr.Zero || result.PoseWorldLandmarks == IntPtr.Zero)
                throw new InvalidDataException("MediaPipe returned a null landmark list.");

            NativeLandmarkList normalizedList = Marshal.PtrToStructure<NativeLandmarkList>(result.PoseLandmarks);
            NativeLandmarkList worldList = Marshal.PtrToStructure<NativeLandmarkList>(result.PoseWorldLandmarks);
            ImmutableArray<MediaPipePoseLandmark> normalized = ConvertLandmarkList(normalizedList, "normalized");
            ImmutableArray<MediaPipePoseLandmark> world = ConvertLandmarkList(worldList, "world");
            return new MediaPipePoseFrame(timestampMilliseconds, normalized, world);
        }

        private static ImmutableArray<MediaPipePoseLandmark> ConvertLandmarkList(
            NativeLandmarkList list,
            string description)
        {
            if (list.LandmarksCount != MediaPipePoseFrame.LandmarkCount || list.Landmarks == IntPtr.Zero)
            {
                throw new InvalidDataException(
                    "MediaPipe returned an invalid " + description + " landmark count.");
            }

            int nativeSize = Marshal.SizeOf<NativeLandmark>();
            ImmutableArray<MediaPipePoseLandmark>.Builder landmarks =
                ImmutableArray.CreateBuilder<MediaPipePoseLandmark>(MediaPipePoseFrame.LandmarkCount);

            for (int i = 0; i < MediaPipePoseFrame.LandmarkCount; i++)
            {
                NativeLandmark native = Marshal.PtrToStructure<NativeLandmark>(
                    IntPtr.Add(list.Landmarks, checked(i * nativeSize)));
                bool hasVisibility = native.HasVisibility != 0;
                bool hasPresence = native.HasPresence != 0;
                float visibility = hasVisibility ? native.Visibility : 0.0f;
                float presence = hasPresence ? native.Presence : 0.0f;

                if (!float.IsFinite(native.X) ||
                    !float.IsFinite(native.Y) ||
                    !float.IsFinite(native.Z) ||
                    !float.IsFinite(visibility) ||
                    !float.IsFinite(presence))
                {
                    throw new InvalidDataException(
                        "MediaPipe returned a non-finite " + description + " landmark.");
                }

                landmarks.Add(new MediaPipePoseLandmark(
                    (MediaPipePoseLandmarkIndex)i,
                    native.X,
                    native.Y,
                    native.Z,
                    hasVisibility,
                    visibility,
                    hasPresence,
                    presence));
            }

            return landmarks.MoveToImmutable();
        }

        private static void ValidateConfidence(float confidence, string parameterName)
        {
            if (!float.IsFinite(confidence) || confidence < 0.0f || confidence > 1.0f)
                throw new ArgumentOutOfRangeException(parameterName, "Confidence must be between zero and one.");
        }

        private static void VerifyPinnedFile(string path, string expectedSha256, string description)
        {
            if (!File.Exists(path))
                throw new FileNotFoundException(description + " was not found.", path);

            using FileStream stream = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                1024 * 1024,
                FileOptions.SequentialScan);
            string actualSha256 = Convert.ToHexString(SHA256.HashData(stream));
            if (!string.Equals(actualSha256, expectedSha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    description + " hash mismatch. Expected " + expectedSha256 +
                    ", got " + actualSha256 + ".");
            }
        }

        private void ThrowIfDisposed()
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeBaseOptions
        {
            public IntPtr ModelAssetBuffer;
            public uint ModelAssetBufferCount;
            public IntPtr ModelAssetPath;
            public int Delegate;
            public int HostEnvironment;
            public int HostSystem;
            public IntPtr HostVersion;
            public IntPtr CaBundlePath;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativePoseLandmarkerOptions
        {
            public NativeBaseOptions BaseOptions;
            public int RunningMode;
            public int NumPoses;
            public float MinimumPoseDetectionConfidence;
            public float MinimumPosePresenceConfidence;
            public float MinimumTrackingConfidence;
            public byte OutputSegmentationMasks;
            public IntPtr ResultCallback;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativePoseLandmarkerResult
        {
            public IntPtr SegmentationMasks;
            public uint SegmentationMasksCount;
            public IntPtr PoseLandmarks;
            public uint PoseLandmarksCount;
            public IntPtr PoseWorldLandmarks;
            public uint PoseWorldLandmarksCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeLandmarkList
        {
            public IntPtr Landmarks;
            public uint LandmarksCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeLandmark
        {
            public float X;
            public float Y;
            public float Z;
            public byte HasVisibility;
            public float Visibility;
            public byte HasPresence;
            public float Presence;
            public IntPtr Name;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CreatePoseLandmarkerDelegate(
            ref NativePoseLandmarkerOptions options,
            ref IntPtr landmarker,
            ref IntPtr errorMessage);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int DetectForVideoDelegate(
            IntPtr landmarker,
            IntPtr image,
            IntPtr imageProcessingOptions,
            long timestampMilliseconds,
            ref NativePoseLandmarkerResult result,
            ref IntPtr errorMessage);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void ClosePoseLandmarkerResultDelegate(ref NativePoseLandmarkerResult result);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int ClosePoseLandmarkerDelegate(IntPtr landmarker, ref IntPtr errorMessage);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CreateImageFromUint8DataDelegate(
            int format,
            int width,
            int height,
            IntPtr pixelData,
            int pixelDataSize,
            ref IntPtr image,
            ref IntPtr errorMessage);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void FreeImageDelegate(IntPtr image);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void FreeErrorDelegate(IntPtr errorMessage);

        private sealed class NativeApi
        {
            private static readonly Lazy<NativeApi> Shared = new Lazy<NativeApi>(
                CreateShared,
                System.Threading.LazyThreadSafetyMode.ExecutionAndPublication);

            private readonly CreatePoseLandmarkerDelegate _createPoseLandmarker;
            private readonly DetectForVideoDelegate _detectForVideo;
            private readonly ClosePoseLandmarkerResultDelegate _closePoseLandmarkerResult;
            private readonly ClosePoseLandmarkerDelegate _closePoseLandmarker;
            private readonly CreateImageFromUint8DataDelegate _createImageFromUint8Data;
            private readonly FreeImageDelegate _freeImage;
            private readonly FreeErrorDelegate _freeError;

            private NativeApi(IntPtr library)
            {
                _createPoseLandmarker = GetExport<CreatePoseLandmarkerDelegate>(library, "MpPoseLandmarkerCreate");
                _detectForVideo = GetExport<DetectForVideoDelegate>(library, "MpPoseLandmarkerDetectForVideo");
                _closePoseLandmarkerResult = GetExport<ClosePoseLandmarkerResultDelegate>(library, "MpPoseLandmarkerCloseResult");
                _closePoseLandmarker = GetExport<ClosePoseLandmarkerDelegate>(library, "MpPoseLandmarkerClose");
                _createImageFromUint8Data = GetExport<CreateImageFromUint8DataDelegate>(library, "MpImageCreateFromUint8Data");
                _freeImage = GetExport<FreeImageDelegate>(library, "MpImageFree");
                _freeError = GetExport<FreeErrorDelegate>(library, "MpErrorFree");
            }

            public static NativeApi Instance => Shared.Value;

            public int CreatePoseLandmarker(
                ref NativePoseLandmarkerOptions options,
                ref IntPtr landmarker,
                ref IntPtr errorMessage) =>
                _createPoseLandmarker(ref options, ref landmarker, ref errorMessage);

            public int DetectForVideo(
                IntPtr landmarker,
                IntPtr image,
                IntPtr imageProcessingOptions,
                long timestampMilliseconds,
                ref NativePoseLandmarkerResult result,
                ref IntPtr errorMessage) =>
                _detectForVideo(
                    landmarker,
                    image,
                    imageProcessingOptions,
                    timestampMilliseconds,
                    ref result,
                    ref errorMessage);

            public void ClosePoseLandmarkerResult(ref NativePoseLandmarkerResult result) =>
                _closePoseLandmarkerResult(ref result);

            public int CreateImageFromUint8Data(
                int format,
                int width,
                int height,
                IntPtr pixelData,
                int pixelDataSize,
                ref IntPtr image,
                ref IntPtr errorMessage) =>
                _createImageFromUint8Data(
                    format,
                    width,
                    height,
                    pixelData,
                    pixelDataSize,
                    ref image,
                    ref errorMessage);

            public void FreeImage(IntPtr image)
            {
                if (image != IntPtr.Zero)
                    _freeImage(image);
            }

            public void FreeError(IntPtr errorMessage)
            {
                if (errorMessage != IntPtr.Zero)
                    _freeError(errorMessage);
            }

            public void ThrowIfFailed(int status, IntPtr errorMessage, string operation)
            {
                string? message = null;
                if (errorMessage != IntPtr.Zero)
                {
                    try
                    {
                        message = Marshal.PtrToStringUTF8(errorMessage);
                    }
                    finally
                    {
                        _freeError(errorMessage);
                    }
                }

                if (status != 0)
                    throw new MediaPipePoseException(operation, status, message);
            }

            public bool ClosePoseLandmarker(IntPtr landmarker)
            {
                IntPtr errorMessage = IntPtr.Zero;
                int status = _closePoseLandmarker(landmarker, ref errorMessage);
                FreeError(errorMessage);
                return status == 0;
            }

            public void CloseOrphanedPoseLandmarker(IntPtr landmarker)
            {
                try
                {
                    ClosePoseLandmarker(landmarker);
                }
                catch
                {
                    // Constructor failure is already propagating; do not mask it.
                }
            }

            private static NativeApi CreateShared()
            {
                if (!OperatingSystem.IsWindows() || IntPtr.Size != 8)
                    throw new PlatformNotSupportedException("The pinned MediaPipe backend requires 64-bit Windows.");

                ValidateAbiSize<NativeBaseOptions>(56, nameof(NativeBaseOptions));
                ValidateAbiSize<NativePoseLandmarkerOptions>(88, nameof(NativePoseLandmarkerOptions));
                ValidateAbiSize<NativePoseLandmarkerResult>(48, nameof(NativePoseLandmarkerResult));
                ValidateAbiSize<NativeLandmarkList>(16, nameof(NativeLandmarkList));
                ValidateAbiSize<NativeLandmark>(40, nameof(NativeLandmark));

                string libraryPath = Path.GetFullPath(DefaultNativeLibraryPath);
                string openCvLibraryPath = Path.GetFullPath(DefaultOpenCvNativeLibraryPath);
                VerifyPinnedFile(
                    openCvLibraryPath,
                    OpenCvNativeLibrarySha256,
                    "MediaPipe OpenCV dependency");
                VerifyPinnedFile(libraryPath, NativeLibrarySha256, "MediaPipe native library");

                // Load the hash-pinned dependency by absolute path first. This keeps
                // Windows from resolving an arbitrary opencv_world3410.dll via PATH.
                IntPtr openCvLibrary = NativeLibrary.Load(openCvLibraryPath);
                try
                {
                    IntPtr library = NativeLibrary.Load(libraryPath);
                    try
                    {
                        return new NativeApi(library);
                    }
                    catch
                    {
                        NativeLibrary.Free(library);
                        throw;
                    }
                }
                catch
                {
                    NativeLibrary.Free(openCvLibrary);
                    throw;
                }

                // Both successful modules stay loaded for process lifetime. Safe handles
                // can therefore call their release functions during finalization.
            }

            private static T GetExport<T>(IntPtr library, string name) where T : Delegate =>
                Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(library, name));

            private static void ValidateAbiSize<T>(int expectedSize, string name) where T : struct
            {
                int actualSize = Marshal.SizeOf<T>();
                if (actualSize != expectedSize)
                {
                    throw new PlatformNotSupportedException(
                        name + " ABI size mismatch. Expected " + expectedSize + ", got " + actualSize + ".");
                }
            }
        }

        private sealed class PoseLandmarkerSafeHandle : SafeHandleZeroOrMinusOneIsInvalid
        {
            private readonly NativeApi _api;

            public PoseLandmarkerSafeHandle(NativeApi api, IntPtr handle)
                : base(true)
            {
                _api = api;
                SetHandle(handle);
            }

            protected override bool ReleaseHandle()
            {
                try
                {
                    return _api.ClosePoseLandmarker(handle);
                }
                catch
                {
                    return false;
                }
            }
        }

        private sealed class MpImageSafeHandle : SafeHandleZeroOrMinusOneIsInvalid
        {
            private readonly NativeApi _api;

            public MpImageSafeHandle(NativeApi api, IntPtr handle)
                : base(true)
            {
                _api = api;
                SetHandle(handle);
            }

            protected override bool ReleaseHandle()
            {
                try
                {
                    _api.FreeImage(handle);
                    return true;
                }
                catch
                {
                    return false;
                }
            }
        }
    }
}
