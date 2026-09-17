using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace OpenCompositeConfigurator
{
    /// <summary>
    /// Reads and writes opencomposite.ini files, preserving comments and structure.
    /// </summary>
    public class IniFile
    {
        private readonly List<IniLine> _lines = new();
        private string _filePath = "";
        private static readonly string[] OrganizedRootSections =
        {
            "general",
            "audio",
            "input",
            "controller_pose",
            "laser_aim",
            "upscaling",
            "fsr",
            "fsr3",
            "dlss",
            "motion_vectors",
            "asw",
            "cas",
            "mip_bias",
            "vrs",
            "debug"
        };

        private static readonly Dictionary<string, string> RootKeySections = new(StringComparer.OrdinalIgnoreCase)
        {
            ["logLevel"] = "debug",
            ["supersampleRatio"] = "general",
            ["renderCustomHands"] = "general",
            ["useLegacyGreyHands"] = "general",
            ["handColour"] = "general",
            ["haptics"] = "general",
            ["menuLaserEnabled"] = "general",
            ["hapticStrength"] = "general",
            ["enableLayers"] = "general",
            ["dx10Mode"] = "general",
            ["preserveControllerProfileOnSleep"] = "general",
            ["enableHiddenMeshFix"] = "general",
            ["hiddenMeshVerticalScale"] = "general",
            ["invertUsingShaders"] = "general",
            ["initUsingVulkan"] = "general",
            ["enableAppRequestedCubemap"] = "general",
            ["threePartSubmit"] = "general",
            ["useViewportStencil"] = "general",
            ["forceConnectedTouch"] = "general",
            ["admitUnknownProps"] = "general",
            ["keyboardText"] = "general",
            ["controllerModel"] = "general",
            ["swapThumbsticks"] = "general",

            ["enableAudioSwitch"] = "audio",
            ["audioDeviceName"] = "audio",

            ["enableInputSmoothing"] = "input",
            ["inputWindowSize"] = "input",
            ["enableControllerSmoothing"] = "input",
            ["posSmoothMinCutoff"] = "input",
            ["posSmoothBeta"] = "input",
            ["rotSmoothMinCutoff"] = "input",
            ["rotSmoothBeta"] = "input",
            ["disableTriggerTouch"] = "input",
            ["disableThumbrestTouch"] = "input",
            ["disableTrackPad"] = "input",
            ["enableVRIKKnucklesTrackPadSupport"] = "input",
            ["leftDeadZoneSize"] = "input",
            ["leftDeadZoneXSize"] = "input",
            ["leftDeadZoneYSize"] = "input",
            ["rightDeadZoneSize"] = "input",
            ["rightDeadZoneXSize"] = "input",
            ["rightDeadZoneYSize"] = "input",
            ["triggerDeadzone"] = "input",
            ["triggerMax"] = "input",
            ["bodyTrackersEnabled"] = "input",
            ["bodyTrackerRoles"] = "input",
            ["networkTrackersEnabled"] = "input",
            ["networkTrackerPort"] = "input",
            ["cameraLegCalibrationEnabled"] = "input",
            ["walkInPlaceEnabled"] = "input",
            ["walkInPlaceSpeed"] = "input",
            ["walkInPlaceActivation"] = "input",
            ["treadmillEnabled"] = "input",
            ["treadmillPort"] = "input",
            ["treadmillFullSpeed"] = "input",
            ["combatHapticShield"] = "input",
            ["combatHapticWeapon"] = "input",
            ["combatHapticBow"] = "input",
            ["combatHapticMagic"] = "input",
            ["combatHapticStrength"] = "input",

            ["adjustTilt"] = "controller_pose",
            ["tilt"] = "controller_pose",
            ["adjustLeftRotation"] = "controller_pose",
            ["leftXRotation"] = "controller_pose",
            ["leftYRotation"] = "controller_pose",
            ["leftZRotation"] = "controller_pose",
            ["adjustRightRotation"] = "controller_pose",
            ["rightXRotation"] = "controller_pose",
            ["rightYRotation"] = "controller_pose",
            ["rightZRotation"] = "controller_pose",
            ["adjustLeftPosition"] = "controller_pose",
            ["leftXPosition"] = "controller_pose",
            ["leftYPosition"] = "controller_pose",
            ["leftZPosition"] = "controller_pose",
            ["adjustRightPosition"] = "controller_pose",
            ["rightXPosition"] = "controller_pose",
            ["rightYPosition"] = "controller_pose",
            ["rightZPosition"] = "controller_pose",

            ["adjustLeftLaserRotation"] = "laser_aim",
            ["leftLaserXRotation"] = "laser_aim",
            ["leftLaserYRotation"] = "laser_aim",
            ["leftLaserZRotation"] = "laser_aim",
            ["adjustRightLaserRotation"] = "laser_aim",
            ["rightLaserXRotation"] = "laser_aim",
            ["rightLaserYRotation"] = "laser_aim",
            ["rightLaserZRotation"] = "laser_aim",
            ["enableLaserSmoothing"] = "laser_aim",
            ["laserPosSmoothMinCutoff"] = "laser_aim",
            ["laserPosSmoothBeta"] = "laser_aim",
            ["laserRotSmoothMinCutoff"] = "laser_aim",
            ["laserRotSmoothBeta"] = "laser_aim",

            ["dlaaEnabled"] = "upscaling",
            ["dlaaLambda"] = "upscaling",
            ["dlaaEpsilon"] = "upscaling",

            ["fsrEnabled"] = "fsr",
            ["fsrNativeAA"] = "fsr",
            ["fsrRenderScale"] = "fsr",
            ["fsrSharpness"] = "fsr",
            ["fsrRadiusEnabled"] = "fsr",
            ["fsrRadius"] = "fsr",

            ["fsr3Sharpness"] = "fsr3",
            ["fsr3JitterScale"] = "fsr3",
            ["fsr3JitterCancellation"] = "fsr3",
            ["fsr3CameraMV"] = "fsr3",
            ["fsr3ViewToMeters"] = "fsr3",
            ["fsr3ReactivenessScale"] = "fsr3",
            ["fsr3ShadingChangeScale"] = "fsr3",
            ["fsr3AccumulationPerFrame"] = "fsr3",
            ["fsr3MinDisocclusionAccumulation"] = "fsr3",
            ["fsr3VelocityFactor"] = "fsr3",
            ["fsr3ReactiveBase"] = "fsr3",
            ["fsr3ReactiveEdgeBoost"] = "fsr3",
            ["fsr3ReactiveColorBoost"] = "fsr3",
            ["fsr3ReactiveColorThreshold"] = "fsr3",
            ["fsr3ReactiveColorScale"] = "fsr3",
            ["fsr3ReactiveDepthFalloffStart"] = "fsr3",
            ["fsr3ReactiveDepthFalloffEnd"] = "fsr3",
            ["fsr3DebugMode"] = "fsr3",
            ["fsr3PostAAEnabled"] = "fsr3",
            ["fsr3PostAALambda"] = "fsr3",
            ["fsr3PostAAEpsilon"] = "fsr3",

            ["dlssEnabled"] = "dlss",
            ["dlssPreset"] = "dlss",
            ["dlssRenderScaleOverride"] = "dlss",
            ["dlssModel"] = "dlss",
            ["dlssRenderPreset"] = "dlss",
            ["dlssModeOverride"] = "dlss",
            ["dlssNgxVerboseLogging"] = "dlss",
            ["dlssSharpness"] = "dlss",
            ["dlssMvScale"] = "dlss",
            ["dlssBiasBase"] = "dlss",
            ["dlssBiasEdgeBoost"] = "dlss",
            ["dlssBiasDepthFalloffStart"] = "dlss",
            ["dlssBiasDepthFalloffEnd"] = "dlss",
            ["dlssJitterScale"] = "dlss",
            ["dlssMipBiasOffset"] = "dlss",

            ["motionVectorsEnabled"] = "motion_vectors",
            ["motionVectorScale"] = "motion_vectors",
            ["actorMV"] = "motion_vectors",

            ["aswEnabled"] = "asw",
            ["aswWarpStrength"] = "asw",
            ["aswRotationScale"] = "asw",
            ["aswTranslationScale"] = "asw",
            ["aswLocoScale"] = "asw",
            ["aswDepthScale"] = "asw",
            ["aswEdgeFadeWidth"] = "asw",
            ["aswNearFadeDepth"] = "asw",
            ["aswDebugMode"] = "asw",

            ["casEnabled"] = "cas",
            ["casSharpness"] = "cas",
            ["blueSkyDefenderEnabled"] = "cas",
            ["blueSkyDefenderLambda"] = "cas",
            ["blueSkyDefenderEpsilon"] = "cas",

            ["mipBiasEnabled"] = "mip_bias",
            ["mipBias"] = "mip_bias",
            ["mipBiasOffset"] = "mip_bias",
            ["fsr3MipBiasOffset"] = "mip_bias",

            ["vrsEnabled"] = "vrs",
            ["vrsEyeTracked"] = "vrs",
            ["foveationDebugRings"] = "vrs",
            ["vrsInnerRadius"] = "vrs",
            ["vrsMidRadius"] = "vrs",
            ["vrsFixedInnerRadius"] = "vrs",
            ["vrsFixedMidRadius"] = "vrs",
            ["vrsEyeInnerRadius"] = "vrs",
            ["vrsEyeMidRadius"] = "vrs",
            ["vrsEyeCustomRates"] = "vrs",
            ["vrsEyeInnerRate"] = "vrs",
            ["vrsEyeMidRate"] = "vrs",
            ["vrsEyeOuterRate"] = "vrs",
            ["vrsCompatibilityMode"] = "vrs",
            ["vrsOuterRadius"] = "vrs",
            ["vrsFavorHorizontal"] = "vrs",

            ["logAllOpenVRCalls"] = "debug",
            ["logGetTrackedProperty"] = "debug",
            ["stopOnSoftAbort"] = "debug"
        };

        public string FilePath => _filePath;

        public void Reset()
        {
            _lines.Clear();
        }

        public void Load(string path)
        {
            _filePath = path;
            _lines.Clear();

            if (!File.Exists(path))
                return;

            string currentSection = "";
            foreach (string rawLine in File.ReadAllLines(path))
            {
                string trimmed = rawLine.Trim();

                if (string.IsNullOrEmpty(trimmed) || trimmed.StartsWith(";") || trimmed.StartsWith("#"))
                {
                    _lines.Add(new IniLine { Raw = rawLine, Type = IniLineType.Comment });
                    continue;
                }

                if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
                {
                    currentSection = trimmed.Substring(1, trimmed.Length - 2).Trim().ToLowerInvariant();
                    _lines.Add(new IniLine { Raw = rawLine, Type = IniLineType.Section, Section = currentSection });
                    continue;
                }

                int eq = trimmed.IndexOf('=');
                if (eq > 0)
                {
                    string key = trimmed.Substring(0, eq).Trim();
                    string val = trimmed.Substring(eq + 1).Trim();
                    _lines.Add(new IniLine
                    {
                        Raw = rawLine,
                        Type = IniLineType.KeyValue,
                        Section = currentSection,
                        Key = key,
                        Value = val
                    });
                }
                else
                {
                    _lines.Add(new IniLine { Raw = rawLine, Type = IniLineType.Comment });
                }
            }
        }

        public string Get(string section, string key, string defaultValue = "")
        {
            section = section.ToLowerInvariant();
            string? value = FindValue(section, key);
            if (value != null)
                return value;

            if (section == "")
            {
                value = FindValue("default", key);
                if (value != null)
                    return value;

                if (RootKeySections.TryGetValue(key, out string? mappedSection))
                {
                    value = FindValue(mappedSection, key);
                    if (value != null)
                        return value;
                }

                foreach (string organizedSection in OrganizedRootSections)
                {
                    value = FindValue(organizedSection, key);
                    if (value != null)
                        return value;
                }
            }

            return defaultValue;
        }

        public void Set(string section, string key, string value)
        {
            section = section.ToLowerInvariant();
            if (section == "")
                section = RootKeySections.GetValueOrDefault(key, "general");

            // Try to update existing key
            foreach (var line in _lines)
            {
                if (line.Type == IniLineType.KeyValue &&
                    line.Section == section &&
                    string.Equals(line.Key, key, StringComparison.OrdinalIgnoreCase))
                {
                    line.Value = value;
                    line.Raw = $"{key}={value}";
                    return;
                }
            }

            // Key doesn't exist — find the section and append, or create section
            int sectionIdx = -1;
            int lastKeyInSection = -1;
            for (int i = 0; i < _lines.Count; i++)
            {
                if (_lines[i].Type == IniLineType.Section && _lines[i].Section == section)
                    sectionIdx = i;

                if (sectionIdx >= 0 && _lines[i].Section == section && _lines[i].Type == IniLineType.KeyValue)
                    lastKeyInSection = i;
            }

            var newLine = new IniLine
            {
                Raw = $"{key}={value}",
                Type = IniLineType.KeyValue,
                Section = section,
                Key = key,
                Value = value
            };

            if (sectionIdx >= 0)
            {
                int insertAt = lastKeyInSection >= 0 ? lastKeyInSection + 1 : sectionIdx + 1;
                _lines.Insert(insertAt, newLine);
            }
            else
            {
                // Create new section
                if (_lines.Count > 0)
                    _lines.Add(new IniLine { Raw = "", Type = IniLineType.Comment });

                _lines.Add(new IniLine
                {
                    Raw = $"[{section}]",
                    Type = IniLineType.Section,
                    Section = section
                });
                _lines.Add(newLine);
            }
        }

        public List<(string key, string value)> GetAllInSection(string section)
        {
            section = section.ToLowerInvariant();
            var result = new List<(string key, string value)>();
            foreach (var line in _lines)
            {
                if (line.Type == IniLineType.KeyValue && line.Section == section)
                    result.Add((line.Key, line.Value));
            }
            return result;
        }

        public void Remove(string section, string key)
        {
            section = section.ToLowerInvariant();
            if (section == "")
            {
                var sections = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "", "default" };
                if (RootKeySections.TryGetValue(key, out string? mappedSection))
                    sections.Add(mappedSection);
                foreach (string organizedSection in OrganizedRootSections)
                    sections.Add(organizedSection);

                _lines.RemoveAll(l =>
                    l.Type == IniLineType.KeyValue &&
                    sections.Contains(l.Section) &&
                    string.Equals(l.Key, key, StringComparison.OrdinalIgnoreCase));
            }
            else
            {
                _lines.RemoveAll(l =>
                    l.Type == IniLineType.KeyValue &&
                    l.Section == section &&
                    string.Equals(l.Key, key, StringComparison.OrdinalIgnoreCase));
            }
        }

        public void ClearSection(string section)
        {
            section = section.ToLowerInvariant();
            _lines.RemoveAll(l =>
                l.Section == section &&
                (l.Type == IniLineType.KeyValue || l.Type == IniLineType.Section));
        }

        public void Save()
        {
            Save(_filePath);
        }

        public void Save(string path)
        {
            // Retired experimental rendering keys are removed on save, not exposed as controls.
            foreach (string key in new[] {
                "aswBufferEnabled", "aswExperimentalMode", "aswCaptureEnabled",
                "aswForceLegacy", "aswConcurrentFrameThread", "aswSpeculativeTrackingLead",
                "aswUpscalerReset", "aswUpscalerReactiveMask",
                "aswForceCustom", "aswFPControllerScale", "aswMVConfidence", "aswMVPixelScale"
            })
                Remove("", key);
            _filePath = path;
            var sb = new StringBuilder();
            foreach (var line in _lines)
                sb.AppendLine(line.Raw);
            File.WriteAllText(path, sb.ToString());
        }

        private enum IniLineType { Comment, Section, KeyValue }

        private string? FindValue(string section, string key)
        {
            foreach (var line in _lines)
            {
                if (line.Type == IniLineType.KeyValue &&
                    line.Section == section &&
                    string.Equals(line.Key, key, StringComparison.OrdinalIgnoreCase))
                {
                    return line.Value;
                }
            }

            return null;
        }

        private class IniLine
        {
            public string Raw = "";
            public IniLineType Type;
            public string Section = "";
            public string Key = "";
            public string Value = "";
        }
    }
}
