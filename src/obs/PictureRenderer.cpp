// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/PictureRenderer.hpp"

#include <util/bmem.h>

namespace tapeloop::obs {
namespace {

// OBS's own YUV conversions are internal to libobs, so the plugin brings its own. The
// matrix rows and the range come from video_format_get_parameters_for_format, as OBS
// uses them for asynchronous sources.
constexpr const char *kEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d luma;
uniform texture2d chroma;
uniform texture2d chroma_v;
uniform float4 to_rgb_r;
uniform float4 to_rgb_g;
uniform float4 to_rgb_b;
uniform float3 range_min;
uniform float3 range_max;
uniform float chroma_shift;

sampler_state planes {
	Filter = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct Vertex {
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

Vertex vertex_main(Vertex v)
{
	Vertex result;
	result.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
	result.uv = v.uv;
	return result;
}

float3 rgb_of(float3 yuv)
{
	float4 clamped = float4(clamp(yuv, range_min, range_max), 1.0);
	return float3(dot(to_rgb_r, clamped), dot(to_rgb_g, clamped), dot(to_rgb_b, clamped));
}

float linear_of(float u)
{
	return (u <= 0.04045) ? (u / 12.92) : pow(mad(u, 1.0 / 1.055, 0.055 / 1.055), 2.4);
}

float4 linear_rgb(float3 rgb)
{
	float3 encoded = saturate(rgb);
	return float4(linear_of(encoded.r), linear_of(encoded.g), linear_of(encoded.b), 1.0);
}

// Chroma is sited left of its pairs of luma samples, as OBS's own encoders write it and
// read it back.
float2 chroma_uv(float2 uv)
{
	return uv + float2(chroma_shift, 0.0);
}

float3 nv12_rgb(Vertex v)
{
	float y = luma.Sample(planes, v.uv).x;
	float2 uv = chroma.Sample(planes, chroma_uv(v.uv)).xy;
	return rgb_of(float3(y, uv));
}

float3 i420_rgb(Vertex v)
{
	float y = luma.Sample(planes, v.uv).x;
	float u = chroma.Sample(planes, chroma_uv(v.uv)).x;
	float w = chroma_v.Sample(planes, chroma_uv(v.uv)).x;
	return rgb_of(float3(y, u, w));
}

float4 pixel_nv12(Vertex v) : TARGET
{
	return float4(nv12_rgb(v), 1.0);
}

float4 pixel_i420(Vertex v) : TARGET
{
	return float4(i420_rgb(v), 1.0);
}

float4 pixel_nv12_linear(Vertex v) : TARGET
{
	return linear_rgb(nv12_rgb(v));
}

float4 pixel_i420_linear(Vertex v) : TARGET
{
	return linear_rgb(i420_rgb(v));
}

technique Nv12
{
	pass
	{
		vertex_shader = vertex_main(v);
		pixel_shader = pixel_nv12(v);
	}
}

technique I420
{
	pass
	{
		vertex_shader = vertex_main(v);
		pixel_shader = pixel_i420(v);
	}
}

technique Nv12Linear
{
	pass
	{
		vertex_shader = vertex_main(v);
		pixel_shader = pixel_nv12_linear(v);
	}
}

technique I420Linear
{
	pass
	{
		vertex_shader = vertex_main(v);
		pixel_shader = pixel_i420_linear(v);
	}
}
)";

uint32_t half(uint32_t size) noexcept
{
	return (size + 1) / 2;
}

} // namespace

PictureRenderer::~PictureRenderer()
{
	clear();
	if (effect_) {
		gs_effect_destroy(effect_);
	}
}

bool PictureRenderer::upload(const decode::Picture &picture) noexcept
{
	ready_ = false;
	if (picture.texture || picture.width == 0 || picture.height == 0) {
		return false;
	}
	// Every plane there, and each row of it within its stride.
	const bool nv12 = picture.layout == decode::PixelLayout::Nv12;
	const std::array<uint32_t, 3> rows = {picture.width, nv12 ? half(picture.width) * 2 : half(picture.width),
					      nv12 ? 0 : half(picture.width)};
	for (size_t plane = 0; plane < (nv12 ? 2u : 3u); ++plane) {
		if (!picture.planes[plane] || picture.strides[plane] < rows[plane]) {
			return false;
		}
	}
	if (!makeEffect() || !makeTextures(picture)) {
		return false;
	}

	const int planes = picture.layout == decode::PixelLayout::Nv12 ? 2 : 3;
	for (int plane = 0; plane < planes; ++plane) {
		const auto at = static_cast<size_t>(plane);
		gs_texture_set_image(planes_[at], picture.planes[at], picture.strides[at], false);
	}
	colors_ = picture;
	// Only the colors are kept; the planes are the decoder's.
	colors_.planes = {};
	ready_ = true;
	return true;
}

bool PictureRenderer::draw(uint32_t width, uint32_t height) noexcept
{
	if (!ready_) {
		return false;
	}
	setColors(colors_);
	drawPlanes(layout_, planes_, colors_.width, width, height);
	return true;
}

bool PictureRenderer::drawNv12(gs_texture_t *luma, gs_texture_t *chroma, const decode::Picture &colors, uint32_t width,
			       uint32_t height) noexcept
{
	if (!luma || !chroma || !makeEffect()) {
		return false;
	}
	decode::Picture nv12 = colors;
	nv12.layout = decode::PixelLayout::Nv12;
	setColors(nv12);
	drawPlanes(decode::PixelLayout::Nv12, {luma, chroma, nullptr}, gs_texture_get_width(luma), width, height);
	return true;
}

void PictureRenderer::drawPlanes(decode::PixelLayout layout, const std::array<gs_texture_t *, 3> &planes,
				 uint32_t lumaWidth, uint32_t width, uint32_t height) noexcept
{
	gs_effect_set_texture(gs_effect_get_param_by_name(effect_, "luma"), planes[0]);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect_, "chroma"), planes[1]);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect_, "chroma_v"), planes[2]);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect_, "to_rgb_r"), &toRgb_[0]);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect_, "to_rgb_g"), &toRgb_[1]);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect_, "to_rgb_b"), &toRgb_[2]);
	gs_effect_set_vec3(gs_effect_get_param_by_name(effect_, "range_min"), &rangeMin_);
	gs_effect_set_vec3(gs_effect_get_param_by_name(effect_, "range_max"), &rangeMax_);
	// Half a luma sample, in texture coordinates.
	gs_effect_set_float(gs_effect_get_param_by_name(effect_, "chroma_shift"),
			    lumaWidth ? 0.5f / static_cast<float>(lumaWidth) : 0.0f);

	// As OBS draws its own asynchronous sources: in linear light when asked to or when the
	// canvas is not plain sRGB, as a 16-bit SDR one is, with the framebuffer encoding it
	// again where it can; otherwise the encoded values as they are, with that encoding
	// off so that they are not encoded twice.
	const bool linear = gs_get_linear_srgb() || gs_get_color_space() != GS_CS_SRGB;
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	const bool nv12 = layout == decode::PixelLayout::Nv12;
	const char *technique = linear ? (nv12 ? "Nv12Linear" : "I420Linear") : (nv12 ? "Nv12" : "I420");
	while (gs_effect_loop(effect_, technique)) {
		gs_draw_sprite(planes[0], 0, width, height);
	}
	gs_enable_framebuffer_srgb(srgb);
}

void PictureRenderer::clear() noexcept
{
	for (gs_texture_t *&plane : planes_) {
		if (plane) {
			gs_texture_destroy(plane);
			plane = nullptr;
		}
	}
	width_ = 0;
	height_ = 0;
	ready_ = false;
}

bool PictureRenderer::makeEffect() noexcept
{
	if (effect_) {
		return true;
	}
	char *error = nullptr;
	effect_ = gs_effect_create(kEffect, "tapeloop-picture.effect", &error);
	if (!effect_) {
		blog(LOG_ERROR, "[tapeloop] The picture effect does not compile: %s",
		     error ? error : "no reason given");
	}
	bfree(error);
	return effect_ != nullptr;
}

bool PictureRenderer::makeTextures(const decode::Picture &picture) noexcept
{
	if (planes_[0] && width_ == picture.width && height_ == picture.height && layout_ == picture.layout) {
		return true;
	}
	clear();
	const uint32_t chromaWidth = half(picture.width);
	const uint32_t chromaHeight = half(picture.height);
	planes_[0] = gs_texture_create(picture.width, picture.height, GS_R8, 1, nullptr, GS_DYNAMIC);
	if (picture.layout == decode::PixelLayout::Nv12) {
		planes_[1] = gs_texture_create(chromaWidth, chromaHeight, GS_R8G8, 1, nullptr, GS_DYNAMIC);
	} else {
		planes_[1] = gs_texture_create(chromaWidth, chromaHeight, GS_R8, 1, nullptr, GS_DYNAMIC);
		planes_[2] = gs_texture_create(chromaWidth, chromaHeight, GS_R8, 1, nullptr, GS_DYNAMIC);
	}
	if (!planes_[0] || !planes_[1] || (picture.layout == decode::PixelLayout::I420 && !planes_[2])) {
		clear();
		return false;
	}
	width_ = picture.width;
	height_ = picture.height;
	layout_ = picture.layout;
	return true;
}

void PictureRenderer::setColors(const decode::Picture &picture) noexcept
{
	video_colorspace space = VIDEO_CS_DEFAULT;
	if (picture.matrix == decode::ColorMatrix::Bt601) {
		space = VIDEO_CS_601;
	} else if (picture.matrix == decode::ColorMatrix::Bt709) {
		space = VIDEO_CS_709;
	}
	const video_range_type range = picture.fullRange ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	const video_format format = picture.layout == decode::PixelLayout::Nv12 ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_I420;

	float matrix[16] = {};
	float minimum[3] = {};
	float maximum[3] = {};
	video_format_get_parameters_for_format(space, range, format, matrix, minimum, maximum);
	for (size_t row = 0; row < toRgb_.size(); ++row) {
		vec4_set(&toRgb_[row], matrix[row * 4], matrix[row * 4 + 1], matrix[row * 4 + 2], matrix[row * 4 + 3]);
	}
	vec3_set(&rangeMin_, minimum[0], minimum[1], minimum[2]);
	vec3_set(&rangeMax_, maximum[0], maximum[1], maximum[2]);
}

} // namespace tapeloop::obs
