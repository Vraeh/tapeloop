// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/RenderAdapter.hpp"

#include <obs.h>

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace tapeloop::obs {

std::optional<uint64_t> renderAdapterLuid() noexcept
{
#ifdef _WIN32
	if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
		return std::nullopt;
	}
	auto *device = static_cast<ID3D11Device *>(gs_get_device_obj());
	Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc = {};
	if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
	    FAILED(adapter->GetDesc(&desc))) {
		return std::nullopt;
	}
	return (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
	       desc.AdapterLuid.LowPart;
#else
	return std::nullopt;
#endif
}

} // namespace tapeloop::obs
