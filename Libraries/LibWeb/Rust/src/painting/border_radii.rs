/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::LengthPercentageRef;
use crate::css::css_pixels::{CssPixelFraction, CssPixels};
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::painting::corner_shapes;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use libgfx_rust::{CornerRadii, CornerRadius, FloatRect};

/// Corner order for `shapes`: top-left, top-right, bottom-right, bottom-left,
/// matching the `values` pairs.
pub(crate) const ROUND_CORNER_SHAPES: [f64; 4] = [1.0, 1.0, 1.0, 1.0];

pub(crate) fn normalize_border_radii_data(
    border_rect: CssPixelRect,
    reference_rect: CssPixelRect,
    corner_radius_pairs: [(LengthPercentageRef<'_>, LengthPercentageRef<'_>); 4],
    corner_shapes: [f64; 4],
) -> BorderRadii {
    scale_radii_to_fit(
        border_rect,
        resolve_corner_radii(reference_rect, corner_radius_pairs, corner_shapes),
    )
}

pub(crate) fn resolve_corner_radii(
    reference_rect: CssPixelRect,
    corner_radius_pairs: [(LengthPercentageRef<'_>, LengthPercentageRef<'_>); 4],
    corner_shapes: [f64; 4],
) -> BorderRadii {
    let mut radii = BorderRadii {
        values: [CssPixels::from_raw(0); 8],
        shapes: corner_shapes,
    };
    for (corner, (horizontal, vertical)) in corner_radius_pairs.into_iter().enumerate() {
        radii.values[corner * 2] = horizontal.to_px(reference_rect.width);
        radii.values[corner * 2 + 1] = vertical.to_px(reference_rect.height);
    }
    radii
}

pub(crate) fn scale_radii_to_fit(border_rect: CssPixelRect, mut radii: BorderRadii) -> BorderRadii {
    let zero = CssPixels::from_raw(0);
    let border_width = border_rect.width.max(zero);
    let border_height = border_rect.height.max(zero);
    for _iteration in 0..2 {
        let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = radii.values;
        let s_top = tl_h + tr_h;
        let s_right = tr_v + br_v;
        let s_bottom = br_h + bl_h;
        let s_left = bl_v + tl_v;

        let mut f = CssPixelFraction::one();
        if s_top > zero && s_top > border_width {
            f = f.min(CssPixelFraction::ratio_of(border_width, s_top));
        }
        if s_right > zero && s_right > border_height {
            f = f.min(CssPixelFraction::ratio_of(border_height, s_right));
        }
        if s_bottom > zero && s_bottom > border_width {
            f = f.min(CssPixelFraction::ratio_of(border_width, s_bottom));
        }
        if s_left > zero && s_left > border_height {
            f = f.min(CssPixelFraction::ratio_of(border_height, s_left));
        }

        if f.is_at_least_one() {
            break;
        }
        for value in &mut radii.values {
            *value = value.mul_by_fraction(f);
        }
    }
    radii
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BorderRadii {
    pub values: [CssPixels; 8],
    /// Superellipse parameters in TL, TR, BR, BL order. `1.0` is `round`;
    /// see `corner_shapes` for the other keywords.
    pub shapes: [f64; 4],
}

impl Default for BorderRadii {
    fn default() -> Self {
        Self {
            values: [CssPixels::from_raw(0); 8],
            shapes: ROUND_CORNER_SHAPES,
        }
    }
}

impl BorderRadii {
    fn corner_present(horizontal: CssPixels, vertical: CssPixels) -> bool {
        horizontal > CssPixels::from_raw(0) && vertical > CssPixels::from_raw(0)
    }

    /// Whether every corner with a radius uses the elliptical `round` shape.
    pub fn is_all_round(&self) -> bool {
        corner_shapes::all_round(&self.shapes)
    }

    /// Whether any corner with a radius uses a non-`round` shape and therefore
    /// needs a superellipse path instead of the elliptical fast path.
    pub fn has_shaped_corners(&self) -> bool {
        if !self.has_any_radius() {
            return false;
        }
        let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = self.values;
        let present = [
            Self::corner_present(tl_h, tl_v),
            Self::corner_present(tr_h, tr_v),
            Self::corner_present(br_h, br_v),
            Self::corner_present(bl_h, bl_v),
        ];
        present
            .into_iter()
            .enumerate()
            .any(|(corner, is_present)| is_present && !corner_shapes::is_round(self.shapes[corner]))
    }

    /// Device-pixel `(horizontal, vertical)` pairs in TL, TR, BR, BL order,
    /// without the integer flooring that [`Self::as_corners`] applies.
    pub fn to_device_floats(&self, converter: &DevicePixelConverter) -> [(f32, f32); 4] {
        let scale = converter.device_pixels_per_css_pixel() as f32;
        let pair = |index: usize| {
            (
                self.values[index].to_float() * scale,
                self.values[index + 1].to_float() * scale,
            )
        };
        [pair(0), pair(2), pair(4), pair(6)]
    }

    /// A superellipse path for `rect` (device pixels), or `None` when every
    /// corner is `round` and the caller should use the elliptical fast path.
    pub fn shaped_path(
        &self,
        rect: libgfx_rust::IntRect,
        converter: &DevicePixelConverter,
    ) -> Option<libgfx_rust::path::OwnedPath> {
        if !self.has_shaped_corners() {
            return None;
        }
        Some(corner_shapes::shaped_rect_path(
            FloatRect::new(rect.x as f32, rect.y as f32, rect.width as f32, rect.height as f32),
            self.to_device_floats(converter),
            self.shapes,
        ))
    }

    pub fn shrink(&mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) {
        let zero = CssPixels::from_raw(0);
        let shrink_one = |radius: &mut CssPixels, by: CssPixels| {
            if *radius != zero {
                *radius = (*radius - by).max(zero);
            }
        };
        shrink_one(&mut self.values[0], left);
        shrink_one(&mut self.values[1], top);
        shrink_one(&mut self.values[2], right);
        shrink_one(&mut self.values[3], top);
        shrink_one(&mut self.values[4], right);
        shrink_one(&mut self.values[5], bottom);
        shrink_one(&mut self.values[6], left);
        shrink_one(&mut self.values[7], bottom);
    }

    pub fn shrunken(mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) -> Self {
        self.shrink(top, right, bottom, left);
        self
    }

    pub fn inflate(&mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) {
        self.shrink(-top, -right, -bottom, -left);
    }

    fn corner(&self, index: usize, converter: &DevicePixelConverter) -> CornerRadius {
        CornerRadius {
            horizontal_radius: converter.floored_device_pixels(self.values[index]),
            vertical_radius: converter.floored_device_pixels(self.values[index + 1]),
        }
    }

    pub fn as_corners(&self, converter: &DevicePixelConverter) -> CornerRadii {
        if !self.has_any_radius() {
            return CornerRadii::default();
        }
        self.corners_unconditionally(converter)
    }

    pub fn corners_unconditionally(&self, converter: &DevicePixelConverter) -> CornerRadii {
        CornerRadii {
            top_left: self.corner(0, converter),
            top_right: self.corner(2, converter),
            bottom_right: self.corner(4, converter),
            bottom_left: self.corner(6, converter),
        }
    }

    pub fn has_any_radius(&self) -> bool {
        let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = self.values;
        Self::corner_present(tl_h, tl_v)
            || Self::corner_present(tr_h, tr_v)
            || Self::corner_present(br_h, br_v)
            || Self::corner_present(bl_h, bl_v)
    }

    pub fn contains(&self, point: CssPixelPoint, rect: CssPixelRect) -> bool {
        if !rect.contains_point(point) {
            return false;
        }
        if !self.has_any_radius() {
            return true;
        }
        if self.has_shaped_corners() {
            // Hit testing runs in CSS pixels; reuse the device-pixel shape
            // test with a 1:1 scale so superellipse corners clip correctly.
            let device_rect = libgfx_rust::FloatRect::new(
                rect.x.to_float(),
                rect.y.to_float(),
                rect.width.to_float(),
                rect.height.to_float(),
            );
            let device_point = libgfx_rust::FloatPoint {
                x: point.x.to_float(),
                y: point.y.to_float(),
            };
            let radii = [
                (self.values[0].to_float(), self.values[1].to_float()),
                (self.values[2].to_float(), self.values[3].to_float()),
                (self.values[4].to_float(), self.values[5].to_float()),
                (self.values[6].to_float(), self.values[7].to_float()),
            ];
            return corner_shapes::shaped_rect_contains(device_point, device_rect, radii, self.shapes);
        }
        let outside_ellipse =
            |horizontal: CssPixels, vertical: CssPixels, center_x: CssPixels, center_y: CssPixels| -> bool {
                let dx = (point.x - center_x).to_double() / horizontal.to_double();
                let dy = (point.y - center_y).to_double() / vertical.to_double();
                dx * dx + dy * dy > 1.0
            };
        let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = self.values;
        if Self::corner_present(tl_h, tl_v) {
            let center_x = rect.left() + tl_h;
            let center_y = rect.top() + tl_v;
            if point.x < center_x && point.y < center_y && outside_ellipse(tl_h, tl_v, center_x, center_y) {
                return false;
            }
        }
        if Self::corner_present(tr_h, tr_v) {
            let center_x = rect.right() - tr_h;
            let center_y = rect.top() + tr_v;
            if point.x > center_x && point.y < center_y && outside_ellipse(tr_h, tr_v, center_x, center_y) {
                return false;
            }
        }
        if Self::corner_present(br_h, br_v) {
            let center_x = rect.right() - br_h;
            let center_y = rect.bottom() - br_v;
            if point.x > center_x && point.y > center_y && outside_ellipse(br_h, br_v, center_x, center_y) {
                return false;
            }
        }
        if Self::corner_present(bl_h, bl_v) {
            let center_x = rect.left() + bl_h;
            let center_y = rect.bottom() - bl_v;
            if point.x < center_x && point.y > center_y && outside_ellipse(bl_h, bl_v, center_x, center_y) {
                return false;
            }
        }
        true
    }
}
