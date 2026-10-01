/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS `corner-shape` (superellipse) geometry.
//!
//! https://drafts.csswg.org/css-borders-4/#corner-shaping
//!
//! The computed `corner-*-shape` values are superellipse parameters `K`
//! (see `compute_corner_shape_parameter`): `round` is 1.0, `squircle` is 2.0,
//! `bevel` is 0.0, `square` is +infinity, `scoop` is -1.0 and `notch` is
//! -infinity. A corner with radii `(rx, ry)` and parameter `K` follows
//! `|1-u|^n + |1-v|^n = 1` with `n = 2^K` in its normalized corner square.
//!
//! This module implements the outer-contour part of that shape: sampling the
//! curve for fills, clips and borders, and point-in-shape tests for hit
//! testing. Inner contours (the padding edge of a border, the shadow edge,
//! ...) reuse the same parameter with shrunk radii, which matches the spec's
//! intent of a nearly-constant-distance inner edge closely enough for convex
//! shapes while staying implementable without the full contour-offset
//! algorithm.

use libgfx_rust::path::{OwnedPath, PathBuilder};
use libgfx_rust::{FloatPoint, FloatRect};

/// Superellipse parameter for `corner-shape: round`.
pub(crate) const ROUND_SHAPE: f64 = 1.0;

/// How close a parameter must be to 1.0 to take the elliptical fast path.
const ROUND_TOLERANCE: f64 = 1e-9;

/// Line segments used to approximate one shaped corner.
const SEGMENTS_PER_CORNER: usize = 96;

pub(crate) fn is_round(shape: f64) -> bool {
    (shape - ROUND_SHAPE).abs() < ROUND_TOLERANCE
}

pub(crate) fn is_square(shape: f64) -> bool {
    shape.is_infinite() && shape.is_sign_positive()
}

pub(crate) fn is_notch(shape: f64) -> bool {
    shape.is_infinite() && shape.is_sign_negative()
}

pub(crate) fn all_round(shapes: &[f64; 4]) -> bool {
    shapes.iter().all(|shape| is_round(*shape))
}

/// The superellipse exponent `n = 2^K`.
fn exponent(shape: f64) -> f64 {
    if shape.is_infinite() {
        if shape.is_sign_positive() { f64::INFINITY } else { 0.0 }
    } else if shape > 1023.0 {
        f64::INFINITY
    } else if shape < -1074.0 {
        0.0
    } else {
        2f64.powf(shape)
    }
}

/// The `v` coordinate of the superellipse at `u`, for exponent `n > 0`.
/// Both inputs and outputs are in `[0, 1]`; the curve runs from `(1, 0)` to
/// `(0, 1)`.
fn superellipse_v(u: f64, n: f64) -> f64 {
    let one_minus_u = (1.0 - u).clamp(0.0, 1.0);
    let inner = 1.0 - one_minus_u.powf(n);
    1.0 - inner.clamp(0.0, 1.0).powf(1.0 / n)
}

/// Whether normalized `(u, v)` is inside a corner with exponent `n > 0`,
/// i.e. on the inner (box-center) side of the curve.
fn superellipse_contains(u: f64, v: f64, n: f64) -> bool {
    let a = (1.0 - u).abs().powf(n);
    let b = (1.0 - v).abs().powf(n);
    a + b <= 1.0 + 1e-9
}

/// Push the interior samples of one corner, walking `u` from `u_start` to
/// `u_end` (one of which is 0.0 and the other 1.0) and mapping each `(u, v)`
/// to device pixels with `map`.
fn push_corner_samples(
    builder: &mut PathBuilder,
    n: f64,
    u_start: f64,
    u_end: f64,
    map: impl Fn(f64, f64) -> (f32, f32),
) {
    for i in 1..SEGMENTS_PER_CORNER {
        let t = i as f64 / SEGMENTS_PER_CORNER as f64;
        let u = u_start + (u_end - u_start) * t;
        let (x, y) = map(u, superellipse_v(u, n));
        builder.line_to(x, y);
    }
}

/// A shaped rectangle path in device pixels.
///
/// `radii` holds `(horizontal, vertical)` pairs in TL, TR, BR, BL order and
/// `shapes` the matching superellipse parameters. Corners with zero radii or
/// `square` shapes stay square; `notch` corners go through the inner center;
/// every other corner is sampled as a superellipse. The path is closed and
/// wound clockwise starting on the top edge.
pub(crate) fn shaped_rect_path(rect: FloatRect, radii: [(f32, f32); 4], shapes: [f64; 4]) -> OwnedPath {
    let mut builder = PathBuilder::new();
    append_shaped_rect(&mut builder, rect, radii, shapes);
    builder.build()
}

/// Append a shaped rectangle subpath to `builder` (see [`shaped_rect_path`]).
/// The subpath is closed; callers combining several subpaths (border rings)
/// should fill the built path with [`WindingRule::EvenOdd`].
pub(crate) fn append_shaped_rect(builder: &mut PathBuilder, rect: FloatRect, radii: [(f32, f32); 4], shapes: [f64; 4]) {
    let left = rect.x;
    let top = rect.y;
    let right = rect.x + rect.width;
    let bottom = rect.y + rect.height;

    let [(tl_h, tl_v), (tr_h, tr_v), (br_h, br_v), (bl_h, bl_v)] = radii;
    let [tl_shape, tr_shape, br_shape, bl_shape] = shapes;

    // Top edge.
    builder.move_to(left + tl_h, top);
    builder.line_to(right - tr_h, top);
    // Top-right: (1, 0) -> (0, 1).
    append_corner(
        builder,
        tr_h,
        tr_v,
        tr_shape,
        (right, top + tr_v),
        (right - tr_h, top + tr_v),
        1.0,
        0.0,
        |u, v| (right - u as f32 * tr_h, top + v as f32 * tr_v),
    );
    // Right edge.
    builder.line_to(right, bottom - br_v);
    // Bottom-right: (0, 1) -> (1, 0).
    append_corner(
        builder,
        br_h,
        br_v,
        br_shape,
        (right - br_h, bottom),
        (right - br_h, bottom - br_v),
        0.0,
        1.0,
        |u, v| (right - u as f32 * br_h, bottom - v as f32 * br_v),
    );
    // Bottom edge.
    builder.line_to(left + bl_h, bottom);
    // Bottom-left: (1, 0) -> (0, 1).
    append_corner(
        builder,
        bl_h,
        bl_v,
        bl_shape,
        (left, bottom - bl_v),
        (left + bl_h, bottom - bl_v),
        1.0,
        0.0,
        |u, v| (left + u as f32 * bl_h, bottom - v as f32 * bl_v),
    );
    // Left edge.
    builder.line_to(left, top + tl_v);
    // Top-left: (0, 1) -> (1, 0).
    append_corner(
        builder,
        tl_h,
        tl_v,
        tl_shape,
        (left + tl_h, top),
        (left + tl_h, top + tl_v),
        0.0,
        1.0,
        |u, v| (left + u as f32 * tl_h, top + v as f32 * tl_v),
    );
    builder.close();
}

/// Append one corner running from the current point (the end of the incoming
/// edge) to `end`, sampling the superellipse from `u_start` to `u_end`.
/// `center` is the inner corner used only by `notch`.
#[allow(clippy::too_many_arguments)]
fn append_corner(
    builder: &mut PathBuilder,
    rx: f32,
    ry: f32,
    shape: f64,
    end: (f32, f32),
    center: (f32, f32),
    u_start: f64,
    u_end: f64,
    map: impl Fn(f64, f64) -> (f32, f32),
) {
    if rx <= 0.0 || ry <= 0.0 || is_square(shape) {
        builder.line_to(end.0, end.1);
        return;
    }
    if is_notch(shape) {
        builder.line_to(center.0, center.1);
        builder.line_to(end.0, end.1);
        return;
    }
    let n = exponent(shape);
    if !n.is_finite() || n <= 0.0 {
        builder.line_to(end.0, end.1);
        return;
    }
    if is_round(shape) {
        builder.elliptical_arc_to(end.0, end.1, rx, ry, 0.0, false, true);
        return;
    }
    push_corner_samples(builder, n, u_start, u_end, map);
    builder.line_to(end.0, end.1);
}

/// Whether `point` is inside the shaped rectangle `rect`.
///
/// `radii` and `shapes` use the same TL, TR, BR, BL order as
/// [`shaped_rect_path`].
pub(crate) fn shaped_rect_contains(
    point: FloatPoint,
    rect: FloatRect,
    radii: [(f32, f32); 4],
    shapes: [f64; 4],
) -> bool {
    if !rect.contains_point(point) {
        return false;
    }
    let left = rect.x;
    let top = rect.y;
    let right = rect.x + rect.width;
    let bottom = rect.y + rect.height;

    // Outer corner, sign towards the box center, per corner.
    let corners = [
        (left, top, 1.0f32, 1.0f32),
        (right, top, -1.0, 1.0),
        (right, bottom, -1.0, -1.0),
        (left, bottom, 1.0, -1.0),
    ];
    for (index, (cx, cy, sx, sy)) in corners.into_iter().enumerate() {
        let (rx, ry) = radii[index];
        if rx <= 0.0 || ry <= 0.0 {
            continue;
        }
        let shape = shapes[index];
        if is_square(shape) {
            continue;
        }
        let min_x = if sx > 0.0 { cx } else { cx - rx };
        let max_x = if sx > 0.0 { cx + rx } else { cx };
        let min_y = if sy > 0.0 { cy } else { cy - ry };
        let max_y = if sy > 0.0 { cy + ry } else { cy };
        if !(point.x >= min_x && point.x <= max_x && point.y >= min_y && point.y <= max_y) {
            continue;
        }
        // Normalized `(u, v)` in [0, 1]² with (0, 0) at the outer corner.
        let u = ((point.x - cx) / sx / rx) as f64;
        let v = ((point.y - cy) / sy / ry) as f64;
        // Points on the inner edges (u == 1 or v == 1) are on the straight
        // edge region and always inside; only the corner square interior is
        // clipped by the curve.
        if u >= 1.0 || v >= 1.0 {
            continue;
        }
        if is_notch(shape) {
            if u + v < 1.0 - 1e-9 {
                return false;
            }
            continue;
        }
        let n = exponent(shape);
        if !n.is_finite() || n <= 0.0 {
            return false;
        }
        if !superellipse_contains(u.clamp(0.0, 1.0), v.clamp(0.0, 1.0), n) {
            return false;
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn approx_eq(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    #[test]
    fn superellipse_endpoints_hold() {
        for shape in [1.0, 2.0, 0.5, 4.0, 0.0, -1.0] {
            let n = exponent(shape);
            assert!(approx_eq(superellipse_v(1.0, n), 0.0));
            assert!(approx_eq(superellipse_v(0.0, n), 1.0));
        }
    }

    #[test]
    fn bevel_is_a_straight_diagonal() {
        let n = exponent(0.0);
        assert!(approx_eq(n, 1.0));
        assert!(approx_eq(superellipse_v(0.25, n), 0.75));
        assert!(approx_eq(superellipse_v(0.5, n), 0.5));
    }

    #[test]
    fn squircle_is_fuller_than_round() {
        let round_v = superellipse_v(0.5, exponent(1.0));
        let squircle_v = superellipse_v(0.5, exponent(2.0));
        assert!(squircle_v < round_v);
    }

    #[test]
    fn scoop_is_emptier_than_round() {
        let round_v = superellipse_v(0.5, exponent(1.0));
        let scoop_v = superellipse_v(0.5, exponent(-1.0));
        assert!(scoop_v > round_v);
    }

    #[test]
    fn round_contains_center_and_clips_outer_pixel() {
        let rect = FloatRect::new(0.0, 0.0, 100.0, 50.0);
        let radii = [(10.0, 10.0); 4];
        let shapes = [1.0; 4];
        assert!(shaped_rect_contains(
            FloatPoint { x: 50.0, y: 25.0 },
            rect,
            radii,
            shapes
        ));
        assert!(!shaped_rect_contains(
            FloatPoint { x: 1.0, y: 1.0 },
            rect,
            radii,
            shapes
        ));
        assert!(shaped_rect_contains(FloatPoint { x: 9.0, y: 9.0 }, rect, radii, shapes));
    }

    #[test]
    fn squircle_contains_more_of_the_corner_than_round() {
        let rect = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        let radii = [(40.0, 40.0); 4];
        let point = FloatPoint { x: 8.0, y: 8.0 };
        assert!(!shaped_rect_contains(point, rect, radii, [1.0; 4]));
        assert!(shaped_rect_contains(point, rect, radii, [2.0; 4]));
    }

    #[test]
    fn square_keeps_the_corner_and_notch_cuts_it() {
        let rect = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        let radii = [(40.0, 40.0); 4];
        let outer = FloatPoint { x: 2.0, y: 2.0 };
        assert!(shaped_rect_contains(outer, rect, radii, [f64::INFINITY; 4]));
        assert!(!shaped_rect_contains(outer, rect, radii, [f64::NEG_INFINITY; 4]));
    }
}
