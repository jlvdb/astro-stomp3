/*
 * field_union.cc
 *
 *  Created on: Aug 11, 2013
 *      Author: scranton
 */

#include "field_union.h"
#include "annulus_bound.h"
#include "pixel_union.h"
#include "point.h"
#include "region_map.h"

namespace s2omp {

field_union::field_union() {
  clear();
}

field_union::~field_union() {
  clear();
}

field_union* field_union::from_bound(const bound_interface& bound, int level,
    FieldType t) {
  field_union* f = new field_union();
  f->init(bound, level, t);

  return f;
}

field_union* field_union::from_field_pixels(const field_vector& pixels,
    FieldType t) {
  field_union* f = new field_union();
  f->init(pixels, t);

  return f;
}

field_union* field_union::from_field_union(const field_union& f, int level) {
  field_union* f_new = new field_union();
  f_new->init(f, level);

  return f_new;
}

bool field_union::init(const bound_interface& bound, int level, FieldType t) {
  clear();
  type_ = t;
  level_ = level;

  pixel_vector pixels;
  bound.get_simple_covering(level, &pixels);

  pixels_.reserve(pixels.size());
  for (pixel_iterator iter = pixels.begin(); iter != pixels.end(); ++iter) {
    double weight = bound.contained_area(*iter);
    pixels_.push_back(field_pixel(iter->id(), 0.0, 0, weight));

    if (type_ != SCALAR_FIELD) {
      area_ += weight;
    } else {
      area_ += iter->exact_area();
    }
  }

  initialized_ = true;
  return initialized_;
}

bool field_union::init(const field_vector& pixels, FieldType t) {
  clear();
  type_ = t;

  pixels_.reserve(pixels.size());
  bool unsorted = false;
  for (field_const_iterator iter = pixels.begin(); iter != pixels.end(); ++iter) {
    // Require all of our field_pixels to have the same level.
    if (pixels_.size() == 0) {
      level_ = iter->level();
    } else if (level_ != iter->level()) {
      std::cout << "s2omp::field_union::init - "
          << "Found mis-matched cell level (" << level_ << "," << iter->level()
          << "); bailing.\n";
      return false;
    }

    // If our input field_vector is not sorted, we need to sort it before
    // we can use it.
    if (pixels_.size() > 0 && iter->id() >= pixels_.back().id()) {
      unsorted = true;
    }

    pixels_.push_back(*iter);

    if (type_ != SCALAR_FIELD) {
      area_ += iter->weight();
    } else {
      area_ += iter->exact_area();
    }

    total_intensity_ += iter->intensity();
    total_points_ += iter->n_points();
  }

  if (unsorted) {
    sort(pixels_.begin(), pixels_.end());
  }

  initialized_ = true;
  return initialized_;
}

bool field_union::init(const field_union& u, int level) {
  if (level > u.level()) {
    std::cout << "s2omp::field_union::init - "
        << "Cannot resample to higher level (" << u.level() << "," << level
        << "); bailing.\n";
    return false;
  }
  clear();
  type_ = u.type();
  level_ = level;

  pixel_vector pixels;
  u.get_simple_covering(level_, &pixels);

  pixels_.reserve(pixels.size());
  for (pixel_iterator iter = pixels.begin(); iter != pixels.end(); ++iter) {
    field_pixel pix = u.resample(*iter);

    pixels_.push_back(pix);

    if (type_ != SCALAR_FIELD) {
      area_ += pix.weight();
    } else {
      area_ += pix.exact_area();
    }

    total_intensity_ += pix.intensity();
    total_points_ += pix.n_points();
  }

  initialized_ = true;
  return initialized_;
}

bool field_union::add_point(const point& p, double intensity) {
  field_iterator iter = lower_bound(pixels_.begin(), pixels_.end(),
      p.to_pixel(level_));
  if (iter == pixels_.end()) {
    return false;
  }

  if (type_ != SCALAR_FIELD) {
    iter->add_to_intensity(intensity, 1);
    total_points_++;
  } else {
    iter->add_to_intensity(intensity, 0);
  }

  total_intensity_ += intensity;

  return true;
}

bool field_union::add_point(const point& p) {
  return add_point(p, p.weight());
}

field_pixel field_union::resample(const pixel& pix) const {
  // If the input pixel level is finer than our map, then return a null
  // field_pixel.
  if (pix.level() > level_) {
    return field_pixel(0LL);
  }

  // If the input pixel doesn't intersect our map, then return an empty
  // field_pixel.
  if (!may_intersect(pix)) {
    return field_pixel(pix.id());
  }

  // Ok, the input pixel is at least partially contained, so we need to
  // figure out the resampling.
  double total_weight = 0.0;
  double total_intensity = 0.0;
  double weighted_intensity = 0.0;
  long total_points = 0;
  long contained_pixels = 0;
  for (pixel c = pix.child_begin(level_); c != pix.child_end(level_); c
      = c.next()) {
    // Look for the child pixels at the level of our field_union.
    field_const_iterator iter = lower_bound(pixels_.begin(), pixels_.end(), c);
    if (iter == pixels_.end())
      continue;

    contained_pixels++;
    total_weight += iter->weight();
    total_intensity += iter->intensity();
    weighted_intensity += iter->weight() * iter->intensity();
    total_points += iter->n_points();
  }

  // If we normalize weighted_intensity by the unmasked fraction, we have an
  // area-averaged value of the intensity over the pixel.
  weighted_intensity /= total_weight;

  // If our pixels were encoding the over-density of the field fields, then
  // we need to convert those values back into the raw values.
  if (converted_to_overdensity_) {
    if (type_ == DENSITY_FIELD) {
      weighted_intensity = weighted_intensity * mean_intensity_
          + mean_intensity_;
    } else {
      weighted_intensity += mean_intensity_;
    }
    if (type_ == DENSITY_FIELD) {
      // For a DENSITY_FIELD, we want to return the total intensity for all of
      // the area subtended by the input pixel.  If we were dealing with a raw
      // map, then that value is already in total intensity.  Since we've
      // got an over-density map, then we need to convert the weighted
      // average into a total by incorporating the unmasked area.
      total_intensity = weighted_intensity * total_weight;
    }
    if (type_ == SAMPLED_FIELD) {
      // Likewise for a SAMPLED_FIELD, but the normalization is based on the
      // total number of points in the pixel.
      total_intensity = weighted_intensity * total_points;
    }
  }

  if (type_ == SCALAR_FIELD) {
    // For a SCALAR_FIELD, we want the average value over the area indicated
    // by the input pixel.
    total_intensity = weighted_intensity;
    total_weight /= contained_pixels;
  }

  return field_pixel(pix.id(), total_intensity, total_weight, total_points);
}

double field_union::find_intensity(const pixel& pix) const {
  return resample(pix).intensity();
}

double field_union::find_density(const pixel& pix) const {
  field_pixel resampled_pix = resample(pix);

  // If the input pixel is inside our map, return the density; 0.0 otherwise.
  return double_gt(resampled_pix.weight(), 0.0) ? resampled_pix.intensity()
      / resampled_pix.weight() : 0.0;
}

double field_union::find_point_density(const pixel& pix) const {
  field_pixel resampled_pix = resample(pix);

  // If the input pixel is inside our map, return the density; 0.0 otherwise.
  return double_gt(resampled_pix.weight(), 0.0) ? resampled_pix.n_points()
      / resampled_pix.weight() : 0.0;
}

double field_union::find_local_area(const bound_interface& bound) const {
  pixel_vector pixels;
  bound.get_center_covering(level_, &pixels);

  double total_area = 0.0;
  for (pixel_iterator iter = pixels.begin(); iter != pixels.end(); ++iter) {
    field_const_iterator s_iter = lower_bound(pixels_.begin(), pixels_.end(),
        *iter);
    if (s_iter == pixels_.end())
      continue;

    if (type_ != SCALAR_FIELD) {
      total_area += s_iter->weight();
    } else {
      total_area += iter->exact_area();
    }
  }

  return total_area;
}

double field_union::find_local_intensity(const bound_interface& bound) const {
  pixel_vector pixels;
  bound.get_center_covering(level_, &pixels);

  double total_intensity = 0.0;
  for (pixel_iterator iter = pixels.begin(); iter != pixels.end(); ++iter) {
    field_const_iterator s_iter = lower_bound(pixels_.begin(), pixels_.end(),
        *iter);
    if (s_iter == pixels_.end())
      continue;

    total_intensity += s_iter->intensity();
  }

  return total_intensity;
}

double field_union::find_local_density(const bound_interface& bound) const {
  pixel_vector pixels;
  bound.get_center_covering(level_, &pixels);

  double total_intensity = 0.0;
  double total_area = 0.0;
  long n_pixels = 0;
  for (pixel_iterator iter = pixels.begin(); iter != pixels.end(); ++iter) {
    field_const_iterator s_iter = lower_bound(pixels_.begin(), pixels_.end(),
        *iter);
    if (s_iter == pixels_.end())
      continue;

    if (type_ != SCALAR_FIELD) {
      total_area += s_iter->weight();
    } else {
      total_area += iter->exact_area();
    }

    total_intensity += s_iter->intensity();
    n_pixels++;
  }

  return n_pixels > 0 ? total_intensity / total_area : 0.0;
}

double field_union::find_local_point_density(const bound_interface& bound) const {
  pixel_vector pixels;
  bound.get_center_covering(level_, &pixels);

  double total_area = 0.0;
  long total_points = 0;
  long n_pixels = 0;
  for (pixel_iterator iter = pixels.begin(); iter != pixels.end(); ++iter) {
    field_const_iterator s_iter = lower_bound(pixels_.begin(), pixels_.end(),
        *iter);
    if (s_iter == pixels_.end())
      continue;

    if (type_ != SCALAR_FIELD) {
      total_area += s_iter->weight();
    } else {
      total_area += iter->exact_area();
    }

    total_points += s_iter->n_points();
    n_pixels++;
  }

  return n_pixels > 0 ? total_points / total_area : 0.0;
}

void field_union::calculate_mean_intensity() {
  double sum_pixel = 0.0;
  mean_intensity_ = 0.0;

  for (field_iterator iter = pixels_.begin(); iter != pixels_.end(); ++iter) {
    switch (type_) {
    case SCALAR_FIELD:
      mean_intensity_ += iter->intensity() * iter->weight();
      sum_pixel += iter->weight();
      break;
    case DENSITY_FIELD:
      mean_intensity_ += iter->intensity() / iter->weight();
      sum_pixel += 1.0;
      break;
    case SAMPLED_FIELD:
      mean_intensity_ += iter->mean_intensity() * iter->weight();
      sum_pixel += iter->weight();
      break;
    }
  }

  mean_intensity_ /= sum_pixel;
  calculated_mean_intensity_ = true;
}

void field_union::convert_to_over_density() {
  if (!calculated_mean_intensity_) {
    calculate_mean_intensity();
  }

  for (field_iterator iter = pixels_.begin(); iter != pixels_.end(); ++iter) {
    switch (type_) {
    case SCALAR_FIELD:
      iter->convert_to_overdensity(mean_intensity_);
      break;
    case DENSITY_FIELD:
      iter->convert_to_fractional_overdensity(mean_intensity_);
      break;
    case SAMPLED_FIELD:
      iter->convert_to_overdensity(mean_intensity_);
      break;
    }
  }

  converted_to_overdensity_ = true;
}

void field_union::convert_from_over_density() {
  if (!calculated_mean_intensity_) {
    std::cout << "s2omp::field_union::convert_from_over_density - "
        << "Cannot convert from overdensity with already knowing the "
        << "mean field density; bailing.\n";
    exit(2);
  }

  for (field_iterator iter = pixels_.begin(); iter != pixels_.end(); ++iter) {
    switch (type_) {
    case SCALAR_FIELD:
      iter->convert_from_overdensity(mean_intensity_);
      break;
    case DENSITY_FIELD:
      iter->convert_from_fractional_overdensity(mean_intensity_);
      break;
    case SAMPLED_FIELD:
      iter->convert_from_overdensity(mean_intensity_);
      break;
    }
  }

  converted_to_overdensity_ = false;
}

// Moving private methods here since it acts as the engine for the various
// correlation methods

bool field_union::correlate_unions(field_union& s, const region_map& regions,
    bool autocorrelate, angular_bin* theta) {
  // Start with a sanity check that we have maps at the same resolution.
  if (!autocorrelate && s.level() != level_) {
    std::cout << "s2omp::field_union::correlate_unions - "
        << "Cannot correlate level " << s.level()
        << " field_union with level " << level_ << " field_union\n";
    return false;
  }

  // If we're regionating the measurement, we need to make sure that the
  // regions are defined at a coarser resolution than our field_unions.
  if (!regions.is_empty() && regions.level() > level_) {
    std::cout << "s2omp::field_union::correlate_unions - "
        << "Cannot use region map at " << regions.level() << " with level "
        << level_ << " field_union\n";
    return false;
  }

  // Convert our field_unions to over-densities if necessary.
  if (!converted_to_overdensity_) {
    convert_to_over_density();
  }
  if (!autocorrelate && !s.is_over_density()) {
    s.convert_to_over_density();
  }

  theta->reset_pixel_wtheta();
  annulus_bound* bound = annulus_bound::from_angular_bin(
      pixels_.begin()->center_point(), *theta);
  for (field_const_iterator iter = pixels_.begin(); iter != pixels_.end(); ++iter) {
    // First, calculate the center covering for an annulus_bound, centered on
    // this pixel.
    pixel_vector covering;
    bound->get_center_covering(level_, &covering);

    // Get the region for this pixel.
    int iter_region = region_map::find_region(regions, *iter);

    // Find the starting and ending points of the covering in the input
    // field_union.  If we're auto-correlating, then the starting point is
    // the next pixel.
    field_const_iterator begin = autocorrelate ? iter + 1 : lower_bound(
        s.begin(), s.end(), covering.front());
    field_const_iterator end =
        upper_bound(s.begin(), s.end(), covering.back());

    // Provided that starting and ending points are on the same face, this is
    // just a matter of iterating through from start to finish and testing
    // each pixel against the bound
    if (end == s.end() || begin->face() == end->face()) {
      for (field_const_iterator cover_iter = begin; cover_iter != end; ++cover_iter) {
        if (bound->contains(cover_iter->center_point())) {
          theta->add_to_pixel_wtheta(iter->intensity() * iter->weight()
              * cover_iter->intensity() * cover_iter->weight(), iter->weight()
              * cover_iter->weight(), iter_region, region_map::find_region(
              regions, *cover_iter));
        }
      }
    } else {
      // If the faces are different, then we risk iterating through a large
      // chunk of the map without finding viable pairs.  In this case, we do a
      // a look-up for each covering pixel.
      for (pixel_iterator cover_iter = covering.begin(); cover_iter
          != covering.end(); ++cover_iter) {
        // Ignore any covering pixels if their ID is less than our starting
        // point.
        if (cover_iter->id() < begin->id())
          continue;

        // Look for the matching field_pixel in our range.  If we find a
        // matching pixel and it's contained in our bound, add it to the tally.
        field_pair range_iter = equal_range(begin, end, *cover_iter);
        if (range_iter.first != range_iter.second && bound->contains(
            range_iter.first->center_point())) {
          theta->add_to_pixel_wtheta(iter->intensity() * iter->weight()
              * range_iter.first->intensity() * range_iter.first->weight(),
              iter->weight() * range_iter.first->weight(), iter_region,
              region_map::find_region(regions, *cover_iter));
        }
      }
    }
  }
  delete bound;

  return true;
}

bool field_union::auto_correlate(angular_bin* theta) {
  return correlate_unions(*this, region_map(), true, theta);
}

bool field_union::auto_correlate(angular_correlation* wtheta) {
  theta_iterator theta_begin = wtheta->begin(level_);
  theta_iterator theta_end = wtheta->end(level_);

  if (theta_begin == theta_end) {
    std::cout << "s2omp::field_union::auto_correlate - "
        << "No angular bins with level = " << level_ << "\n";
    return false;
  }

  for (theta_iterator iter = theta_begin; iter != theta_end; ++iter) {
    if (!auto_correlate(&(*iter))) {
      return false;
    }
  }

  return true;
}

bool field_union::auto_correlate_with_regions(const region_map& regions,
    angular_bin* theta) {
  return correlate_unions(*this, regions, true, theta);
}

bool field_union::auto_correlate_with_regions(const region_map& regions,
    angular_correlation* wtheta) {
  theta_iterator theta_begin = wtheta->begin(level_);
  theta_iterator theta_end = wtheta->end(level_);

  if (theta_begin == theta_end) {
    std::cout << "s2omp::field_union::auto_correlate - "
        << "No angular bins with level = " << level_ << "\n";
    return false;
  }

  for (theta_iterator iter = theta_begin; iter != theta_end; ++iter) {
    if (!auto_correlate_with_regions(regions, &(*iter))) {
      return false;
    }
  }

  return true;
}

bool field_union::cross_correlate(field_union& s, angular_bin* theta) {
  return correlate_unions(s, region_map(), false, theta);
}

bool field_union::cross_correlate(field_union& s, angular_correlation* wtheta) {
  theta_iterator theta_begin = wtheta->begin(level_);
  theta_iterator theta_end = wtheta->end(level_);

  if (theta_begin == theta_end) {
    std::cout << "s2omp::field_union::cross_correlate - "
        << "No angular bins with level = " << level_ << "\n";
    return false;
  }

  for (theta_iterator iter = theta_begin; iter != theta_end; ++iter) {
    if (!cross_correlate(s, &(*iter))) {
      return false;
    }
  }

  return true;
}

bool field_union::cross_correlate_with_regions(field_union& s,
    const region_map& regions, angular_bin* theta) {
  return correlate_unions(s, regions, false, theta);
}

bool field_union::cross_correlate_with_regions(field_union& s,
    const region_map& regions, angular_correlation* wtheta) {
  theta_iterator theta_begin = wtheta->begin(level_);
  theta_iterator theta_end = wtheta->end(level_);

  if (theta_begin == theta_end) {
    std::cout << "s2omp::field_union::cross_correlate_with_regions - "
        << "No angular bins with level = " << level_ << "\n";
    return false;
  }

  for (theta_iterator iter = theta_begin; iter != theta_end; ++iter) {
    if (!cross_correlate_with_regions(s, regions, &(*iter))) {
      return false;
    }
  }

  return true;
}

void field_union::clear() {
  pixels_.clear();
  area_ = 0.0;
  mean_intensity_ = 0.0;
  unmasked_fraction_minimum_ = 1.0e-3;
  total_intensity_ = 0.0;
  level_ = -1;
  total_points_ = 0;
  initialized_ = false;
  initialized_bound_ = false;
  converted_to_overdensity_ = false;
  calculated_mean_intensity_ = false;
}

bool field_union::contains(const point& p) const {
  return contains(p.to_pixel());
}

bool field_union::contains(const pixel& pix) const {
  // This is already done pretty elegantly in S2 so we'll just adapt it
  // for stomp.
  field_const_iterator iter = lower_bound(pixels_.begin(), pixels_.end(), pix);
  if (iter != pixels_.end() && iter->range_min() <= pix)
    return true;
  return iter != pixels_.begin() && (--iter)->range_max() >= pix;
}

double field_union::contained_area(const pixel& pix) const {
  // If the input pixel doesn't intersect our pixels, then we're done.
  if (!may_intersect(pix)) {
    return 0.0;
  }

  // If the input pixel is either on the level of the union or finer, then
  // we just need to find the containing field_pixel.  If we have a
  // SCALAR_FIELD, then the area is equal to the input pixel area.  If it's
  // a DENSITY_FIELD or a SAMPLED_FIELD, then we use the fractional weight
  // to scale the output.
  if (pix.level() >= level_) {
    field_const_iterator iter = lower_bound(pixels_.begin(), pixels_.end(),
        pix.parent(level_));
    double total_area = pix.exact_area();
    if (type_ != SCALAR_FIELD) {
      total_area *= iter->weight() / iter->exact_area();
    }

    return total_area;
  }

  // Finally, if the pixel is coarser than our field_pixels, then we need
  // to determine which children are in our union and aggregate their total
  // area.
  field_const_iterator start = lower_bound(pixels_.begin(), pixels_.end(),
      pix.child_begin(level_));
  field_const_iterator finish = lower_bound(pixels_.begin(), pixels_.end(),
      pix.child_end(level_));

  double total_area = 0.0;
  for (field_const_iterator iter = start; iter != finish; ++iter) {
    if (pix.contains(*iter)) {
      total_area += type_ == SCALAR_FIELD ? iter->exact_area() : iter->weight();
    }
  }

  return total_area;
}

bool field_union::may_intersect(const pixel& pix) const {
  // Per S2, this is exact.
  field_const_iterator iter = lower_bound(pixels_.begin(), pixels_.end(), pix);
  if (iter != end() && iter->range_min() <= pix.range_max())
    return true;
  return iter != begin() && (--iter)->range_max() >= pix.range_min();
}

void field_union::initialize_bound() {
  if (pixels_.empty()) {
    bound_ = circle_bound();
  }

  initialized_bound_ = true;
  bound_ = get_bound();
}

circle_bound field_union::get_bound() const {
  if (initialized_bound_) {
    return bound_;
  }

  circle_bound bound = circle_bound(get_center(), 0.0);
  for (field_const_iterator iter = pixels_.begin(); iter != pixels_.end(); ++iter) {
    bound.add_circle_bound(iter->get_bound());
  }

  return bound;
}

point field_union::get_center() const {
  if (initialized_bound_) {
    return bound_.axis();
  }

  double x = 0.0, y = 0.0, z = 0.0;
  for (field_const_iterator iter = pixels_.begin(); iter != pixels_.end(); ++iter) {
    double area = type_ == DENSITY_FIELD ? iter->weight() : iter->exact_area();
    point center = iter->center_point();
    x += area * center.unit_sphere_x();
    y += area * center.unit_sphere_y();
    z += area * center.unit_sphere_z();
  }

  return point(x, y, z, 1.0);
}

void field_union::get_covering(pixel_vector* pixels) const {
  // this is the default mode for a pixel covering. For this as for other
  // coverings we use only 8 pixels at max to cover our union.
  get_size_covering(DEFAULT_COVERING_PIXELS, pixels);
}

void field_union::get_size_covering(
    const long max_pixels, pixel_vector* pixels) const {
  // For this class we want to keep as few pixels as possible (defined by
  // max_pixels) and retain a close approximation of the area contained by the
  // union.
  if (!pixels->empty())
    pixels->clear();
  double average_area = area_ / (1.0 * max_pixels);
  int level = MAX_LEVEL;

  while (pixel::average_area(level) < average_area) {
    if (level == 0)
      break;
    level--;
  }
  if (level < MAX_LEVEL)
    level++;

  while (pixels->empty() || pixels->size() > max_pixels) {
    pixels->clear();
    get_simple_covering(level, pixels);
    level--;
  }
}

void field_union::get_area_covering(double fractional_area_tolerance,
    pixel_vector* pixels) const {
  if (!pixels->empty())
    pixels->clear();
  pixels->reserve(pixels_.size());

  for (field_const_iterator iter = pixels_.begin();
      iter != pixels_.end(); ++iter) {
    pixels->push_back(iter->parent(level_));
  }

  pixel_union pix_union;
  pix_union.init(*pixels);
  pix_union.get_size_covering(fractional_area_tolerance, pixels);
}

void field_union::get_interior_covering(int max_level, pixel_vector* pixels) const {
  if (!pixels->empty())
    pixels->clear();
  pixels->reserve(pixels_.size());

  for (field_const_iterator iter = pixels_.begin(); iter != pixels_.end(); ++iter) {
    pixels->push_back(iter->parent(level_));
  }

  pixel_union pix_union;
  pix_union.init(*pixels);
  pix_union.get_interior_covering(max_level, pixels);
}

void field_union::get_simple_covering(int level, pixel_vector* pixels) const {
  // For a simple covering of a pixel_union we don't need anything fancy like
  // FloodFill from S2 (at least for this method). If we want a simple covering
  // of a pixel_union then we just need to loop through the pixels, make parents
  // out of children
  if (!pixels->empty())
    pixels->clear();

  for (field_const_iterator iter = pixels_.begin(); iter != pixels_.end(); ++iter) {
    if (iter->level() < level) {
      for (pixel c = iter->child_begin(); c != iter->child_end(); c = c.next()) {
        if (!pixels->empty() && pixels->back().contains(c))
          continue;
        pixels->push_back(c);
      }
    } else {
      pixel pix = iter->parent(level);
      if (!pixels->empty() && pixels->back().contains(pix))
        continue;
      pixels->push_back(pix);
    }
  }
}

void field_union::get_center_covering(int level, pixel_vector* pixels) const {
  get_simple_covering(level, pixels);
}

} // end namespace s2omp
