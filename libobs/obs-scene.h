/******************************************************************************
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

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

#pragma once

#include "obs.h"
#include "obs-internal.h"
#include "graphics/matrix4.h"

/* how obs scene! */

struct obs_scene_item {
	volatile long         ref;
	volatile bool         removed;

	struct obs_scene      *parent;
	struct obs_source     *source;
	bool                  visible;
	bool                  selected;

	struct vec2           pos;
	struct vec2           scale;
	float                 rot;
	uint32_t              align;

	/* last width/height of the source, this is used to check whether
	 * ths transform needs updating */
	uint32_t              last_width;
	uint32_t              last_height;

	struct matrix4        box_transform;
	struct matrix4        draw_transform;

	enum obs_bounds_type  bounds_type;
	uint32_t              bounds_align;
	struct vec2           bounds;

	obs_hotkey_pair_id    toggle_visibility;

	/* would do **prev_next, but not really great for reordering */
	struct obs_scene_item *prev;
	struct obs_scene_item *next;
};

struct obs_scene {
	struct obs_source     *source;

	pthread_mutex_t       mutex;
	struct obs_scene_item *first_item;
};
