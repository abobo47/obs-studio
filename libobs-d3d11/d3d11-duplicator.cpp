/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "d3d11-subsystem.hpp"
#include <unordered_map>
#include <util/platform.h>

static inline bool get_monitor(gs_device_t *device, int monitor_idx, IDXGIOutput **dxgiOutput)
{
	HRESULT hr;

	hr = device->adapter->EnumOutputs(monitor_idx, dxgiOutput);
	if (FAILED(hr)) {
		if (hr == DXGI_ERROR_NOT_FOUND) {
			return false;
		}

		throw HRError("Failed to get output", hr);
	}

	return true;
}

void gs_duplicator::StartQueued()
{
	ComPtr<IDXGIOutput5> output5;
	ComPtr<IDXGIOutput1> output1;
	ComPtr<IDXGIOutput> output;
	HRESULT hr;

	/* already running (device rebuild): consumer side is re-opened lazily */
	if (duplicator)
		return;

	if (!get_monitor(device, idx, output.Assign())) {
		throw "Invalid monitor index";
	}

	hr = D3D11CreateDevice(device->adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			       nullptr, 0, D3D11_SDK_VERSION, prod_device.Assign(), nullptr, prod_context.Assign());
	if (FAILED(hr)) {
		throw HRError("Failed to create capture device", hr);
	}

	hr = output->QueryInterface(IID_PPV_ARGS(output5.Assign()));
	hdr = false;
	sdr_white_nits = 80.f;

	bool ok = false;
	if (SUCCEEDED(hr)) {
		constexpr DXGI_FORMAT supportedFormats[]{
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R10G10B10A2_UNORM,
			DXGI_FORMAT_B8G8R8A8_UNORM,
		};
		hr = output5->DuplicateOutput1(prod_device, 0, _countof(supportedFormats), supportedFormats,
					       duplicator.Assign());
		if (SUCCEEDED(hr)) {
			ok = true;
		} else {
			blog(LOG_WARNING, "queued duplicator: DuplicateOutput1 failed (%08lX), monitor %d", hr, idx);
		}
		DXGI_OUTPUT_DESC desc;
		if (SUCCEEDED(output->GetDesc(&desc))) {
			gs_monitor_color_info info = device->GetMonitorColorInfo(desc.Monitor);
			hdr = info.hdr;
			sdr_white_nits = (float)info.sdr_white_nits;
		}
	}

	if (!ok) {
		hr = output->QueryInterface(IID_PPV_ARGS(output1.Assign()));
		if (FAILED(hr)) {
			throw HRError("Failed to query IDXGIOutput1", hr);
		}

		hr = output1->DuplicateOutput(prod_device, duplicator.Assign());
		if (FAILED(hr)) {
			throw HRError("Failed to DuplicateOutput", hr);
		}
		blog(LOG_INFO, "queued duplicator: using legacy DuplicateOutput, monitor %d", idx);
	}

	delay_ns = 17000000;
	if (const char *e = getenv("OBS_DUP_DELAY_MS")) {
		double ms = atof(e);
		if (ms >= 0.0 && ms <= 200.0)
			delay_ns = (uint64_t)(ms * 1000000.0);
	}
	blog(LOG_INFO, "queued duplicator: monitor %d, delay %.1f ms", idx, delay_ns / 1000000.0);

	stop = false;
	lost = false;
	thread = std::thread([this]() { Thread(); });
}

void gs_duplicator::StartLegacy()
{
	ComPtr<IDXGIOutput5> output5;
	ComPtr<IDXGIOutput1> output1;
	ComPtr<IDXGIOutput> output;
	HRESULT hr;

	if (!get_monitor(device, idx, output.Assign())) {
		throw "Invalid monitor index";
	}

	hr = output->QueryInterface(IID_PPV_ARGS(output5.Assign()));
	hdr = false;
	sdr_white_nits = 80.f;
	if (SUCCEEDED(hr)) {
		constexpr DXGI_FORMAT supportedFormats[]{
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R10G10B10A2_UNORM,
			DXGI_FORMAT_B8G8R8A8_UNORM,
		};
		hr = output5->DuplicateOutput1(device->device, 0, _countof(supportedFormats), supportedFormats,
					       duplicator.Assign());
		if (FAILED(hr)) {
			blog(LOG_WARNING, "queued duplicator: legacy DuplicateOutput1 on OBS device failed (%08lX)", hr);
			throw HRError("Failed to DuplicateOutput1", hr);
		}
		DXGI_OUTPUT_DESC desc;
		if (SUCCEEDED(output->GetDesc(&desc))) {
			gs_monitor_color_info info = device->GetMonitorColorInfo(desc.Monitor);
			hdr = info.hdr;
			sdr_white_nits = (float)info.sdr_white_nits;
		}
	} else {
		hr = output->QueryInterface(IID_PPV_ARGS(output1.Assign()));
		if (FAILED(hr)) {
			throw HRError("Failed to query IDXGIOutput1", hr);
		}

		hr = output1->DuplicateOutput(device->device, duplicator.Assign());
		if (FAILED(hr)) {
			throw HRError("Failed to DuplicateOutput", hr);
		}
	}
	legacy = true;
}

void gs_duplicator::Start()
{
	/* already running (device rebuild) */
	if (duplicator)
		return;

	legacy = false;
	if (!want_queue) {
		StartLegacy();
		blog(LOG_INFO, "DXGI duplicator: monitor %d ACTIVE MODE = LEGACY (stock capture, queue switched off)", idx);
		return;
	}
	HRESULT why = S_OK;
	try {
		StartQueued();
		blog(LOG_INFO, "DXGI duplicator: monitor %d ACTIVE MODE = QUEUE (timestamped frame queue)", idx);
		return;
	} catch (const HRError &e) {
		why = e.hr;
	} catch (const char *) {
		why = E_FAIL;
	}

	/* second device is refused: go back to the original single-device path */
	duplicator.Clear();
	prod_context.Clear();
	prod_device.Clear();
	blog(LOG_WARNING, "queued duplicator: separate-device capture failed (%08lX), monitor %d; trying OBS device",
	     why, idx);
	StartLegacy();
	blog(LOG_WARNING, "DXGI duplicator: monitor %d ACTIVE MODE = LEGACY (queue was requested but FAILED, fallback)",
	     idx);
}

void gs_duplicator::Release()
{
	if (legacy) {
		duplicator.Release();
		return;
	}
	/* OBS device is going away: drop everything opened on it */
	std::lock_guard<std::mutex> g(lock);
	queue.clear();
	for (auto &s : ring) {
		s.cons_mutex.Clear();
		s.cons_tex.Clear();
	}
}

bool gs_duplicator::EnsureRing(const D3D11_TEXTURE2D_DESC &src)
{
	if (!ring.empty() && ring_format == src.Format && ring_w == src.Width && ring_h == src.Height)
		return true;

	std::vector<gs_dup_slot> fresh(6);
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = src.Width;
	d.Height = src.Height;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = src.Format;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	for (auto &s : fresh) {
		ComPtr<IDXGIResource> res;
		if (FAILED(prod_device->CreateTexture2D(&d, nullptr, s.prod_tex.Assign())))
			return false;
		if (FAILED(s.prod_tex->QueryInterface(IID_PPV_ARGS(s.prod_mutex.Assign()))))
			return false;
		if (FAILED(s.prod_tex->QueryInterface(IID_PPV_ARGS(res.Assign()))))
			return false;
		if (FAILED(res->GetSharedHandle(&s.handle)))
			return false;
	}

	std::lock_guard<std::mutex> g(lock);
	queue.clear();
	ring.swap(fresh);
	ring_format = src.Format;
	ring_w = src.Width;
	ring_h = src.Height;
	write_idx = 0;
	return true;
}

void gs_duplicator::Thread()
{
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);

	bool first = true;
	while (!stop) {
		DXGI_OUTDUPL_FRAME_INFO info;
		ComPtr<IDXGIResource> res;
		ComPtr<ID3D11Texture2D> tex;

		HRESULT hr = duplicator->AcquireNextFrame(100, &info, res.Assign());
		if (hr == DXGI_ERROR_WAIT_TIMEOUT)
			continue;
		if (FAILED(hr)) {
			blog(LOG_WARNING, "queued duplicator: AcquireNextFrame failed (%08lX), monitor %d", hr, idx);
			lost = true;
			return;
		}

		/* LastPresentTime == 0: only the mouse moved, image unchanged */
		if (info.LastPresentTime.QuadPart == 0 && !first) {
			duplicator->ReleaseFrame();
			continue;
		}
		if (info.LastPresentTime.QuadPart == 0) {
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			info.LastPresentTime = now;
		}
		first = false;

		if (FAILED(res->QueryInterface(IID_PPV_ARGS(tex.Assign())))) {
			duplicator->ReleaseFrame();
			continue;
		}

		D3D11_TEXTURE2D_DESC desc;
		tex->GetDesc(&desc);

		if (!EnsureRing(desc)) {
			blog(LOG_WARNING, "queued duplicator: shared ring creation failed (%ux%u fmt %d), monitor %d",
			     desc.Width, desc.Height, (int)desc.Format, idx);
			duplicator->ReleaseFrame();
			lost = true;
			return;
		}

		if (!logged_first) {
			logged_first = true;
			blog(LOG_INFO, "queued duplicator: first frame %ux%u fmt %d, monitor %d", desc.Width,
			     desc.Height, (int)desc.Format, idx);
		}

		int slot = write_idx;
		gs_dup_slot &s = ring[slot];
		bool copied = false;
		if (SUCCEEDED(s.prod_mutex->AcquireSync(0, 8))) {
			prod_context->CopyResource(s.prod_tex, tex);
			s.prod_mutex->ReleaseSync(0);
			prod_context->Flush();
			copied = true;
		}
		duplicator->ReleaseFrame();

		if (!copied) {
			stat_dropped++;
			continue;
		}

		LONGLONG q = info.LastPresentTime.QuadPart;
		uint64_t ts = (uint64_t)(q / freq.QuadPart) * 1000000000ULL +
			      (uint64_t)((q % freq.QuadPart) * 1000000000ULL / freq.QuadPart);

		std::lock_guard<std::mutex> g(lock);
		queue.push_back({slot, ts});
		write_idx = (write_idx + 1) % (int)ring.size();
		while (queue.size() > 4) {
			queue.pop_front();
			stat_dropped++;
		}
		stat_frames++;
		if (info.AccumulatedFrames > 1)
			stat_coalesced += info.AccumulatedFrames - 1;
	}
}

gs_duplicator::gs_duplicator(gs_device_t *device_, int monitor_idx, bool queued_)
	: gs_obj(device_, gs_type::gs_duplicator),
	  texture(nullptr),
	  idx(monitor_idx),
	  refs(1),
	  updated(false)
{
	want_queue = queued_;
	key = monitor_idx | (queued_ ? 0x10000 : 0);
	Start();
}

gs_duplicator::~gs_duplicator()
{
	stop = true;
	if (thread.joinable())
		thread.join();
	delete texture;
}

extern "C" {

EXPORT bool device_get_duplicator_monitor_info(gs_device_t *device, int monitor_idx, struct gs_monitor_info *info)
{
	DXGI_OUTPUT_DESC desc;
	HRESULT hr;

	try {
		ComPtr<IDXGIOutput> output;

		if (!get_monitor(device, monitor_idx, output.Assign())) {
			return false;
		}

		hr = output->GetDesc(&desc);
		if (FAILED(hr)) {
			throw HRError("GetDesc failed", hr);
		}

	} catch (const HRError &error) {
		blog(LOG_ERROR,
		     "device_get_duplicator_monitor_info: "
		     "%s (%08lX)",
		     error.str, error.hr);
		return false;
	}

	switch (desc.Rotation) {
	case DXGI_MODE_ROTATION_UNSPECIFIED:
	case DXGI_MODE_ROTATION_IDENTITY:
		info->rotation_degrees = 0;
		break;

	case DXGI_MODE_ROTATION_ROTATE90:
		info->rotation_degrees = 90;
		break;

	case DXGI_MODE_ROTATION_ROTATE180:
		info->rotation_degrees = 180;
		break;

	case DXGI_MODE_ROTATION_ROTATE270:
		info->rotation_degrees = 270;
		break;
	}

	info->x = desc.DesktopCoordinates.left;
	info->y = desc.DesktopCoordinates.top;
	info->cx = desc.DesktopCoordinates.right - info->x;
	info->cy = desc.DesktopCoordinates.bottom - info->y;

	return true;
}

EXPORT int device_duplicator_get_monitor_index(gs_device_t *device, void *monitor)
{
	const HMONITOR handle = (HMONITOR)monitor;

	int index = -1;

	UINT output = 0;
	while (index == -1) {
		IDXGIOutput *pOutput;
		const HRESULT hr = device->adapter->EnumOutputs(output, &pOutput);
		if (hr == DXGI_ERROR_NOT_FOUND) {
			break;
		}

		if (SUCCEEDED(hr)) {
			DXGI_OUTPUT_DESC desc;
			if (SUCCEEDED(pOutput->GetDesc(&desc))) {
				if (desc.Monitor == handle) {
					index = output;
				}
			} else {
				blog(LOG_ERROR,
				     "device_duplicator_get_monitor_index: "
				     "Failed to get desc (%08lX)",
				     hr);
			}

			pOutput->Release();
		} else if (hr == DXGI_ERROR_NOT_FOUND) {
			blog(LOG_ERROR,
			     "device_duplicator_get_monitor_index: "
			     "Failed to get output (%08lX)",
			     hr);
		}

		++output;
	}

	return index;
}

static std::unordered_map<int, gs_duplicator *> instances;

void reset_duplicators(void)
{
	for (std::pair<const int, gs_duplicator *> &pair : instances) {
		pair.second->updated = false;
	}
}

EXPORT gs_duplicator_t *device_duplicator_create_ex(gs_device_t *device, int monitor_idx, bool queued)
{
	gs_duplicator *duplicator = nullptr;
	const int key = monitor_idx | (queued ? 0x10000 : 0);

	blog(LOG_INFO, "DXGI duplicator: create requested, monitor %d, requested mode = %s", monitor_idx,
	     queued ? "QUEUE" : "LEGACY");

	const auto it = instances.find(key);
	if (it != instances.end()) {
		duplicator = it->second;
		duplicator->refs++;
		blog(LOG_INFO, "DXGI duplicator: monitor %d, reusing existing %s duplicator (refs=%d)", monitor_idx,
		     queued ? "QUEUE" : "LEGACY", duplicator->refs);
		return duplicator;
	}

	try {
		duplicator = new gs_duplicator(device, monitor_idx, queued);
		instances[key] = duplicator;

	} catch (const char *error) {
		blog(LOG_WARNING, "device_duplicator_create: %s (monitor %d)", error, monitor_idx);
		return nullptr;

	} catch (const HRError &error) {
		blog(LOG_WARNING, "device_duplicator_create: %s (%08lX) (monitor %d)", error.str, error.hr, monitor_idx);
		return nullptr;
	}

	return duplicator;
}

EXPORT gs_duplicator_t *device_duplicator_create(gs_device_t *device, int monitor_idx)
{
	return device_duplicator_create_ex(device, monitor_idx, false);
}

EXPORT void gs_duplicator_destroy(gs_duplicator_t *duplicator)
{
	if (--duplicator->refs == 0) {
		blog(LOG_INFO, "DXGI duplicator: monitor %d destroyed (was %s)", duplicator->idx,
		     duplicator->legacy ? "LEGACY" : "QUEUE");
		instances.erase(duplicator->key);
		delete duplicator;
	}
}

static inline void copy_texture(gs_duplicator_t *d, ID3D11Texture2D *tex)
{
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);
	const gs_color_format format = ConvertDXGITextureFormat(desc.Format);
	const gs_color_format general_format = gs_generalize_format(format);

	if (!d->texture || (d->texture->width != desc.Width) || (d->texture->height != desc.Height) ||
	    (d->texture->format != general_format)) {

		delete d->texture;
		d->texture = (gs_texture_2d *)gs_texture_create(desc.Width, desc.Height, general_format, 1, nullptr, 0);
		d->color_space =
			d->hdr ? GS_CS_709_SCRGB
			       : ((desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? GS_CS_SRGB_16F : GS_CS_SRGB);
	}

	if (d->texture) {
		d->device->context->CopyResource(d->texture->texture, tex);
	}
}

static inline void dup_tick_stats(gs_duplicator *d, uint64_t now)
{
	if (d->last_tick_ns) {
		const uint64_t gap = now - d->last_tick_ns;
		if (gap > d->max_tick_ns)
			d->max_tick_ns = gap;
		if (gap > 25000000ULL)
			d->stat_late++;
	}
	d->last_tick_ns = now;
}

EXPORT bool gs_duplicator_update_frame(gs_duplicator_t *d)
{
	if (!d->duplicator) {
		return false;
	}
	if (d->updated) {
		return true;
	}
	{
		const uint64_t tnow = os_gettime_ns();
		dup_tick_stats(d, tnow);
		if (d->legacy && tnow - d->leg_last_log > 10000000000ULL) {
			if (d->leg_last_log)
				blog(LOG_INFO,
				     "queued duplicator: LEGACY 10s frames=%llu coalesced=%llu late=%llu maxgap=%.1fms",
				     (unsigned long long)d->leg_frames, (unsigned long long)d->leg_coalesced,
				     (unsigned long long)d->stat_late, d->max_tick_ns / 1000000.0);
			d->leg_frames = d->leg_coalesced = 0;
			d->stat_late = 0;
			d->max_tick_ns = 0;
			d->leg_last_log = tnow;
		}
	}
	if (d->legacy) {
		DXGI_OUTDUPL_FRAME_INFO info;
		ComPtr<ID3D11Texture2D> tex;
		ComPtr<IDXGIResource> res;
		HRESULT hr = d->duplicator->AcquireNextFrame(0, &info, res.Assign());
		if (hr == DXGI_ERROR_ACCESS_LOST) {
			return false;
		} else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
			return true;
		} else if (FAILED(hr)) {
			blog(LOG_ERROR, "gs_duplicator_update_frame: Failed to update frame (%08lX)", hr);
			return false;
		}
		if (info.LastPresentTime.QuadPart != 0) {
			d->leg_frames++;
			if (info.AccumulatedFrames > 1)
				d->leg_coalesced += info.AccumulatedFrames - 1;
		}
		hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)tex.Assign());
		if (FAILED(hr)) {
			d->duplicator->ReleaseFrame();
			return true;
		}
		copy_texture(d, tex);
		d->duplicator->ReleaseFrame();
		d->updated = true;
		return true;
	}
	if (d->lost) {
		return false;
	}

	/* FIFO resampler: one output frame per OBS tick, taken from frames
	 * that are at least delay_ns old. Repeat only if none is ready, drop
	 * one only when 3+ are waiting. */
	ComPtr<ID3D11Texture2D> src;
	ComPtr<IDXGIKeyedMutex> mtx;
	bool have_entry = false;
	{
		std::lock_guard<std::mutex> g(d->lock);
		const uint64_t now = os_gettime_ns();
		const uint64_t target = now > d->delay_ns ? now - d->delay_ns : 0;

		size_t avail = 0;
		for (const auto &e : d->queue) {
			if (e.ts_ns <= target)
				avail++;
			else
				break;
		}

		if (avail == 0) {
			d->stat_dups++;
		} else {
			if (avail >= 3) {
				d->queue.pop_front();
				d->stat_skips++;
			}
			const gs_dup_entry e = d->queue.front();
			d->queue.pop_front();
			have_entry = true;

			if (e.slot < (int)d->ring.size()) {
				gs_dup_slot &s = d->ring[e.slot];
				if (!s.cons_tex) {
					HRESULT hr = d->device->device->OpenSharedResource(
						s.handle, IID_PPV_ARGS(s.cons_tex.Assign()));
					if (SUCCEEDED(hr))
						hr = s.cons_tex->QueryInterface(IID_PPV_ARGS(s.cons_mutex.Assign()));
					if (FAILED(hr)) {
						s.cons_tex.Clear();
						s.cons_mutex.Clear();
						blog(LOG_ERROR, "queued duplicator: OpenSharedResource failed (%08lX)",
						     hr);
					}
				}
				src = s.cons_tex;
				mtx = s.cons_mutex;
			}
		}

		if (now - d->stat_last_log > 10000000000ULL) {
			if (d->stat_last_log)
				blog(LOG_INFO,
				     "queued duplicator: 10s frames=%llu repeats=%llu drops=%llu coalesced=%llu overflow=%llu copyfail=%llu late=%llu maxgap=%.1fms",
				     (unsigned long long)d->stat_frames, (unsigned long long)d->stat_dups,
				     (unsigned long long)d->stat_skips, (unsigned long long)d->stat_coalesced,
				     (unsigned long long)d->stat_dropped,
				     (unsigned long long)d->stat_copy_fail, (unsigned long long)d->stat_late,
				     d->max_tick_ns / 1000000.0);
			d->stat_late = 0;
			d->max_tick_ns = 0;
			d->stat_copy_fail = 0;
			d->stat_frames = d->stat_dups = d->stat_skips = d->stat_coalesced = d->stat_dropped = 0;
			d->stat_last_log = now;
		}
	}

	if (have_entry) {
		HRESULT shr = E_FAIL;
		if (src && mtx) {
			shr = mtx->AcquireSync(0, 8);
			if (SUCCEEDED(shr)) {
				copy_texture(d, src);
				mtx->ReleaseSync(0);
			}
		}
		if (FAILED(shr)) {
			if (d->stat_copy_fail++ == 0)
				blog(LOG_WARNING, "queued duplicator: copy from shared frame failed (%08lX, %s)",
				     shr, src ? "acquire" : "open");
		}
	}

	d->updated = true;
	return true;
}

EXPORT gs_texture_t *gs_duplicator_get_texture(gs_duplicator_t *duplicator)
{
	return duplicator->texture;
}

EXPORT enum gs_color_space gs_duplicator_get_color_space(gs_duplicator_t *duplicator)
{
	return duplicator->color_space;
}

EXPORT float gs_duplicator_get_sdr_white_level(gs_duplicator_t *duplicator)
{
	return duplicator->sdr_white_nits;
}
}
