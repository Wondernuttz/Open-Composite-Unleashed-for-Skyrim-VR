using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Numerics;
using OpenCompositeConfigurator;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>
    /// Dependency-free deterministic checks for the World3D CSV schema and
    /// streaming lifecycle. These do not require a camera, MediaPipe native
    /// binaries, Skyrim, or a test framework.
    /// </summary>
    public static class World3DCalibrationRecorderSelfTests
    {
        public static IReadOnlyList<string> RunAll()
        {
            var failures = new List<string>();
            Run("World3D CSV schema and raw values", TestSchemaAndRawValues, failures);
            Run("World3D CSV invariant culture and quoting", TestInvariantCultureAndQuoting, failures);
            Run("World3D CSV missing converted frame", TestMissingConvertedFrame, failures);
            Run("World3D recorder streaming lifecycle", TestRecorderLifecycle, failures);
            Run("World3D leg derivation symmetry and continuity", TestLegDerivation, failures);
            return failures.AsReadOnly();
        }

        public static void AssertAll()
        {
            IReadOnlyList<string> failures = RunAll();
            if (failures.Count != 0)
                throw new InvalidOperationException(
                    "World3D calibration recorder self-test failed:\n" + string.Join("\n", failures));
        }

        private static void TestSchemaAndRawValues()
        {
            World3DCalibrationRecord record = CreateRecord(includeConverted: true);
            (List<string> header, List<string> row) = Serialize(record);

            const int expectedColumns = 15
                + WorldLandmarkFrame.MediaPipeLandmarkCount * 8
                + 9
                + (BodyTrackerFrame.TrackerSlotCount + 1) * 10
                + 2 * 40;
            Require(header.Count == expectedColumns,
                $"Expected {expectedColumns} columns, got {header.Count}.");
            Require(row.Count == header.Count,
                $"Header has {header.Count} columns but row has {row.Count}.");
            Require(new HashSet<string>(header, StringComparer.Ordinal).Count == header.Count,
                "CSV header contains duplicate column names.");

            var fields = Index(header, row);
            Require(fields["source_sequence"] == "17", "Source sequence was not serialized.");
            Require(fields["source_timestamp_ms"] == "1234", "Source timestamp was not serialized.");
            Require(fields["source_epoch"] == "9", "Source epoch was not serialized.");
            Require(fields["mp_27_left_ankle_world_x_m"] == "27.125",
                "Raw left-ankle World3D X was not preserved.");
            Require(fields["mp_27_left_ankle_image_y_norm"] == "0.29",
                "Normalized left-ankle image Y was not preserved.");
            Require(fields["mp_05_right_eye_valid"] == "0",
                "Raw landmark validity was not preserved.");
            Require(fields["mp_05_right_eye_confidence"] == "0.55",
                "Raw landmark confidence was not preserved.");
            Require(fields["converted_present"] == "1", "Converted frame presence was lost.");
            Require(fields["converted_matches_source"] == "1",
                "Matching source/converted identity was not recorded.");
            Require(fields["head_position_x_m"] == "1.25",
                "Converted head position was not serialized.");
            Require(fields["tracker_03_position_z_m"] == "3.75",
                "Converted tracker slot data was not serialized.");
            Require(fields["left_state_code"] == "6"
                && fields["left_strike_bearing_valid"] == "1",
                "Caller-supplied left leg state/strike fields were not serialized.");
            Require(fields["right_state_code"] == "5"
                && fields["right_blocks_gait"] == "0",
                "Caller-supplied right leg fields were not serialized.");
        }

        private static void TestInvariantCultureAndQuoting()
        {
            CultureInfo previousCulture = CultureInfo.CurrentCulture;
            CultureInfo previousUiCulture = CultureInfo.CurrentUICulture;
            try
            {
                CultureInfo french = CultureInfo.GetCultureInfo("fr-FR");
                CultureInfo.CurrentCulture = french;
                CultureInfo.CurrentUICulture = french;

                (List<string> header, List<string> row) = Serialize(CreateRecord(includeConverted: true));
                var fields = Index(header, row);
                Require(fields["head_position_x_m"] == "1.25",
                    "A current-culture decimal separator leaked into the CSV.");
                Require(fields["view_label"] == "left, 45 degrees",
                    "A comma-containing label did not round-trip through CSV quoting.");
                Require(fields["action_label"] == "kick \"hard\"",
                    "A quoted label did not round-trip through CSV escaping.");
                Require(fields["utc_iso8601"].EndsWith("Z", StringComparison.Ordinal),
                    "UTC timestamp was not emitted in round-trip ISO-8601 form.");
            }
            finally
            {
                CultureInfo.CurrentCulture = previousCulture;
                CultureInfo.CurrentUICulture = previousUiCulture;
            }
        }

        private static void TestMissingConvertedFrame()
        {
            (List<string> header, List<string> row) = Serialize(CreateRecord(includeConverted: false));
            Require(row.Count == header.Count,
                "A missing converted frame changed the fixed CSV column count.");
            var fields = Index(header, row);
            Require(fields["converted_present"] == "0", "Missing converted frame was not marked absent.");
            Require(fields["converted_sequence"].Length == 0,
                "Missing converted metadata was written as a plausible value.");
            Require(fields["head_position_valid"].Length == 0,
                "Missing converted head pose was written as a plausible pose.");
            Require(fields["left_state_label"] == "kick \"hard\"",
                "Leg diagnostics shifted columns when converted output was absent.");
        }

        private static void TestRecorderLifecycle()
        {
            string path = Path.Combine(
                Path.GetTempPath(),
                "ocu_world3d_capture_" + Guid.NewGuid().ToString("N") + ".csv");
            try
            {
                using var recorder = new World3DCalibrationRecorder();
                recorder.Start(path);
                Require(recorder.IsRecording, "Recorder did not enter its recording state.");
                Require(string.Equals(recorder.FilePath, Path.GetFullPath(path), StringComparison.OrdinalIgnoreCase),
                    "Recorder did not expose its active full path.");

                bool secondStartRejected = false;
                try { recorder.Start(path + ".second"); }
                catch (InvalidOperationException) { secondStartRejected = true; }
                Require(secondStartRejected, "Recorder accepted two simultaneous output streams.");

                World3DCalibrationRecord record = CreateRecord(includeConverted: true);
                recorder.Append(
                    record.Labels,
                    record.Landmarks,
                    record.ConvertedFrame,
                    record.LeftLeg,
                    record.RightLeg);
                recorder.Stop();
                recorder.Stop();
                Require(!recorder.IsRecording, "Stop did not leave the recorder idle.");

                string[] lines = File.ReadAllLines(path);
                Require(lines.Length == 2, $"Streamed capture contained {lines.Length} lines instead of two.");
                Require(ParseCsvLine(lines[0]).Count == ParseCsvLine(lines[1]).Count,
                    "Streamed header/row column counts differ.");
            }
            finally
            {
                try { File.Delete(path); } catch { }
                try { File.Delete(path + ".second"); } catch { }
            }
        }

        private static void TestLegDerivation()
        {
            var deriver = new World3DLegCaptureDeriver();
            CameraLowerBodyClassification classification = CreateClassification(1100);
            World3DLegCapturePair first = deriver.Derive(
                CreateAnatomicalFrame(1, 1000, 31, Vector3.Zero, Vector3.Zero),
                classification,
                stateSource: "world3d");
            Require(first.Left.IsValid && first.Right.IsValid,
                "Canonical 3D legs did not produce valid diagnostic geometry.");
            Require(!first.Left.FootVelocityValid && !first.Right.FootVelocityValid,
                "First frame claimed a velocity without prior-frame continuity.");
            Require(MathF.Abs(first.Left.HipKneeLengthMeters
                    - first.Right.HipKneeLengthMeters) <= 0.00001f,
                "Symmetric legs produced asymmetric thigh lengths.");
            Require(MathF.Abs(first.Left.KneeAnkleLengthMeters
                    - first.Right.KneeAnkleLengthMeters) <= 0.00001f,
                "Symmetric legs produced asymmetric shin lengths.");

            World3DLegCapturePair moving = deriver.Derive(
                CreateAnatomicalFrame(
                    2,
                    1100,
                    31,
                    new Vector3(0f, 0f, 0.1f),
                    new Vector3(0f, 0f, -0.1f)),
                classification,
                stateSource: "world3d");
            Require(moving.Left.HipVelocityValid
                && moving.Left.KneeVelocityValid
                && moving.Left.AnkleVelocityValid
                && moving.Left.FootVelocityValid,
                "Continuous left-leg frame did not expose valid joint velocities.");
            Require(moving.Right.HipVelocityValid
                && moving.Right.KneeVelocityValid
                && moving.Right.AnkleVelocityValid
                && moving.Right.FootVelocityValid,
                "Continuous right-leg frame did not expose valid joint velocities.");
            AssertNear(moving.Left.FootVelocityMetersPerSecond,
                new Vector3(0f, 0f, 1f), 0.0001f, "left foot velocity");
            AssertNear(moving.Right.FootVelocityMetersPerSecond,
                new Vector3(0f, 0f, -1f), 0.0001f, "right foot velocity");
            Require(moving.Left.StateCode == (int)CameraLegMotionState.KickExtend
                && moving.Left.StateSource == "world3d"
                && moving.Left.StateTimestampMs == 1100,
                "Left RTMW semantic state provenance was not retained.");
            Require(moving.Right.StateCode == (int)CameraLegMotionState.WalkStep,
                "Right RTMW semantic state was not retained.");

            World3DLegCapturePair newEpoch = deriver.Derive(
                CreateAnatomicalFrame(
                    3,
                    1200,
                    32,
                    new Vector3(0f, 0f, 0.2f),
                    new Vector3(0f, 0f, -0.2f)),
                CreateClassification(1200),
                stateSource: "world3d");
            Require(!newEpoch.Left.FootVelocityValid && !newEpoch.Right.FootVelocityValid,
                "Source-epoch reset leaked a cross-session foot velocity.");
        }

        private static World3DCalibrationRecord CreateRecord(bool includeConverted)
        {
            var landmarks = new WorldLandmark[WorldLandmarkFrame.MediaPipeLandmarkCount];
            for (int i = 0; i < landmarks.Length; i++)
            {
                landmarks[i] = new WorldLandmark(
                    new Vector3(0.01f + i * 0.01f, 0.02f + i * 0.01f, -0.5f - i * 0.01f),
                    new Vector3(i + 0.125f, -i - 0.25f, i * 0.5f + 0.75f),
                    0.5f + i * 0.01f,
                    isValid: i != 5);
            }

            var source = new WorldLandmarkFrame(
                sequence: 17,
                timestampMs: 1234,
                sourceEpoch: 9,
                sourceFlags: BodyPoseSourceFlags.NativeWorker,
                imageWidth: 1280,
                imageHeight: 720,
                inputMirrored: true,
                landmarks);

            BodyTrackerFrame? converted = includeConverted ? CreateConvertedFrame() : null;
            var labels = new World3DCalibrationLabels(
                segment: 4,
                view: "left, 45 degrees",
                action: "kick \"hard\"",
                leg: "both",
                take: 2);
            var left = new World3DLegCaptureFields(
                isValid: true,
                stateCode: 6,
                stateLabel: "kick \"hard\"",
                stateSource: "rtmw_2d",
                stateTimestampMs: 1230,
                stateConfidence: 0.91f,
                baselineReady: true,
                blocksGait: true,
                stepOnset: false,
                anatomicalFlipRejected: false,
                groundReferenceValid: true,
                groundReferenceYMeters: -0.92f,
                kneeAngleDegrees: 164.5f,
                straightness: 0.96f,
                hipKneeLengthMeters: 0.44f,
                kneeAnkleLengthMeters: 0.45f,
                ankleLiftMeters: 0.32f,
                footLiftMeters: 0.30f,
                horizontalShinMeters: 0.28f,
                strikeBearingValid: true,
                strikeBearing: new Vector3(-0.4f, 0f, 0.9165151f),
                strikeWeight: 0.8f,
                hipVelocityValid: true,
                hipVelocityMetersPerSecond: new Vector3(0.01f, 0f, 0f),
                kneeVelocityValid: true,
                kneeVelocityMetersPerSecond: new Vector3(0.2f, 0.1f, 0.4f),
                ankleVelocityValid: true,
                ankleVelocityMetersPerSecond: new Vector3(0.4f, 0.1f, 1.1f),
                footVelocityValid: true,
                footVelocityMetersPerSecond: new Vector3(0.42f, 0.08f, 1.2f));
            var right = new World3DLegCaptureFields(
                isValid: true,
                stateCode: 5,
                stateLabel: "walk_step",
                stateSource: "rtmw_2d",
                stateTimestampMs: 1230,
                stateConfidence: 0.73f,
                baselineReady: true,
                blocksGait: false,
                stepOnset: true,
                groundReferenceValid: true,
                groundReferenceYMeters: -0.92f,
                kneeAngleDegrees: 142f,
                straightness: 0.78f,
                hipKneeLengthMeters: 0.43f,
                kneeAnkleLengthMeters: 0.44f,
                ankleLiftMeters: 0.08f,
                footLiftMeters: 0.06f,
                horizontalShinMeters: 0.07f,
                footVelocityMetersPerSecond: new Vector3(0.05f, 0.1f, 0.2f));

            return new World3DCalibrationRecord(
                captureTimestampMs: 5000,
                captureElapsedMs: 250,
                utcTimestamp: new DateTimeOffset(2026, 8, 1, 12, 34, 56, TimeSpan.FromHours(-4)),
                labels,
                source,
                converted,
                left,
                right);
        }

        private static BodyTrackerFrame CreateConvertedFrame()
        {
            var trackers = new BodyTrackerPose[BodyTrackerFrame.TrackerSlotCount];
            for (int i = 0; i < trackers.Length; i++)
            {
                trackers[i] = new BodyTrackerPose(
                    new Vector3(i + 1.25f, i + 1.5f, i + 1.75f),
                    Quaternion.CreateFromAxisAngle(Vector3.UnitY, 0.05f * (i + 1)),
                    0.9f - i * 0.02f,
                    positionValid: true,
                    rotationValid: true);
            }

            return new BodyTrackerFrame(
                sequence: 17,
                timestampMs: 1234,
                sourceEpoch: 9,
                source: BodyTrackerSource.Continuous3D,
                sourceFlags: BodyPoseSourceFlags.Continuous3D
                    | BodyPoseSourceFlags.MediaPipe33
                    | BodyPoseSourceFlags.MetricWorldCoordinates,
                head: new BodyTrackerPose(
                    new Vector3(1.25f, 1.75f, -0.5f),
                    Quaternion.Identity,
                    0.95f,
                    positionValid: true,
                    rotationValid: true),
                trackers);
        }

        private static CameraLowerBodyClassification CreateClassification(long timestampMs)
        {
            var left = new CameraLegClassification(
                CameraLegMotionState.KickExtend,
                kneeAngleDegrees: 164f,
                confidence: 0.92f,
                isValid: true,
                baselineReady: true,
                anatomicalFlipRejected: false,
                blocksGait: true,
                stepOnset: false,
                normalizedAnkleLift: 0.35f,
                normalizedKneeLift: 0.3f,
                normalizedReach: 1.2f,
                extensionRatePerSecond: 0.8f,
                normalizedDepthDelta: 0.2f,
                neutralKneeDropNormalized: 0.5f,
                neutralAnkleDropNormalized: 1f,
                neutralLiftMeters: 0f);
            var right = new CameraLegClassification(
                CameraLegMotionState.WalkStep,
                kneeAngleDegrees: 145f,
                confidence: 0.8f,
                isValid: true,
                baselineReady: true,
                anatomicalFlipRejected: false,
                blocksGait: false,
                stepOnset: true,
                normalizedAnkleLift: 0.08f,
                normalizedKneeLift: 0.05f,
                normalizedReach: 0.95f,
                extensionRatePerSecond: 0.1f,
                normalizedDepthDelta: 0.04f,
                neutralKneeDropNormalized: 0.5f,
                neutralAnkleDropNormalized: 1f,
                neutralLiftMeters: 0f);
            return new CameraLowerBodyClassification(
                timestampMs,
                left,
                right,
                coordinatedGait: false,
                gaitConfidence: 0.6f);
        }

        private static WorldLandmarkFrame CreateAnatomicalFrame(
            long sequence,
            long timestampMs,
            uint epoch,
            Vector3 leftOffset,
            Vector3 rightOffset)
        {
            var landmarks = new WorldLandmark[WorldLandmarkFrame.MediaPipeLandmarkCount];

            void Put(MediaPipePoseLandmarkIndex index, Vector3 ocuPosition)
            {
                // MediaPipe world X already matches anatomical right; Y/Z do not.
                Vector3 rawWorld = new Vector3(
                    ocuPosition.X, -ocuPosition.Y, -ocuPosition.Z);
                landmarks[(int)index] = new WorldLandmark(
                    normalizedPosition: new Vector3(0.5f, 0.5f, 0f),
                    worldPosition: rawWorld,
                    confidence: 1f,
                    isValid: true);
            }

            Put(MediaPipePoseLandmarkIndex.LeftHip,
                new Vector3(-0.2f, 0.9f, 0f) + leftOffset);
            Put(MediaPipePoseLandmarkIndex.LeftKnee,
                new Vector3(-0.2f, 0.45f, 0f) + leftOffset);
            Put(MediaPipePoseLandmarkIndex.LeftAnkle,
                new Vector3(-0.2f, 0f, 0f) + leftOffset);
            Put(MediaPipePoseLandmarkIndex.LeftHeel,
                new Vector3(-0.2f, 0f, -0.1f) + leftOffset);
            Put(MediaPipePoseLandmarkIndex.LeftFootIndex,
                new Vector3(-0.2f, 0f, 0.15f) + leftOffset);

            Put(MediaPipePoseLandmarkIndex.RightHip,
                new Vector3(0.2f, 0.9f, 0f) + rightOffset);
            Put(MediaPipePoseLandmarkIndex.RightKnee,
                new Vector3(0.2f, 0.45f, 0f) + rightOffset);
            Put(MediaPipePoseLandmarkIndex.RightAnkle,
                new Vector3(0.2f, 0f, 0f) + rightOffset);
            Put(MediaPipePoseLandmarkIndex.RightHeel,
                new Vector3(0.2f, 0f, -0.1f) + rightOffset);
            Put(MediaPipePoseLandmarkIndex.RightFootIndex,
                new Vector3(0.2f, 0f, 0.15f) + rightOffset);

            return new WorldLandmarkFrame(
                sequence,
                timestampMs,
                epoch,
                BodyPoseSourceFlags.NativeWorker,
                imageWidth: 1280,
                imageHeight: 720,
                inputMirrored: false,
                landmarks);
        }

        private static (List<string> Header, List<string> Row) Serialize(
            World3DCalibrationRecord record)
        {
            using var writer = new StringWriter(CultureInfo.InvariantCulture)
            {
                NewLine = "\n",
            };
            World3DCalibrationCsvSerializer.WriteHeader(writer);
            World3DCalibrationCsvSerializer.WriteRow(writer, record);
            string[] lines = writer.ToString().Split('\n', StringSplitOptions.RemoveEmptyEntries);
            Require(lines.Length == 2, $"Serializer emitted {lines.Length} physical lines.");
            return (ParseCsvLine(lines[0]), ParseCsvLine(lines[1]));
        }

        private static Dictionary<string, string> Index(
            IReadOnlyList<string> header,
            IReadOnlyList<string> row)
        {
            var result = new Dictionary<string, string>(header.Count, StringComparer.Ordinal);
            for (int i = 0; i < header.Count; i++)
                result.Add(header[i], row[i]);
            return result;
        }

        private static List<string> ParseCsvLine(string line)
        {
            var values = new List<string>();
            var value = new System.Text.StringBuilder();
            bool quoted = false;
            for (int i = 0; i < line.Length; i++)
            {
                char current = line[i];
                if (quoted)
                {
                    if (current == '"')
                    {
                        if (i + 1 < line.Length && line[i + 1] == '"')
                        {
                            value.Append('"');
                            i++;
                        }
                        else
                        {
                            quoted = false;
                        }
                    }
                    else
                    {
                        value.Append(current);
                    }
                }
                else if (current == ',')
                {
                    values.Add(value.ToString());
                    value.Clear();
                }
                else if (current == '"' && value.Length == 0)
                {
                    quoted = true;
                }
                else
                {
                    value.Append(current);
                }
            }
            Require(!quoted, "CSV row ended inside a quoted field.");
            values.Add(value.ToString());
            return values;
        }

        private static void Run(string name, Action test, List<string> failures)
        {
            try { test(); }
            catch (Exception exception) { failures.Add(name + ": " + exception.Message); }
        }

        private static void Require(bool condition, string message)
        {
            if (!condition)
                throw new InvalidOperationException(message);
        }

        private static void AssertNear(
            Vector3 actual,
            Vector3 expected,
            float tolerance,
            string label)
        {
            if (Vector3.Distance(actual, expected) > tolerance)
                throw new InvalidOperationException(
                    $"{label}: expected {expected}, got {actual}.");
        }
    }
}
