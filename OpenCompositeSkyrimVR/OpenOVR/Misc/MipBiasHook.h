#pragma once

#include <d3d11.h>

bool InitMipBiasHook(ID3D11DeviceContext* ctx);
void ConfigureMipBiasHook(bool enabled, float lodBias);
void ShutdownMipBiasHook();
bool IsMipBiasHookActive();

