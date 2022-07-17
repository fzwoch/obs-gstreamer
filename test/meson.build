#
# obs-gstreamer. OBS Studio plugin.
# Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
#
# This file is part of obs-gstreamer.
#
# obs-gstreamer is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# obs-gstreamer is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
#

project('obs', 'c',
    default_options : [
        'b_asneeded=false',
    ],
)

executable('obs',
    'main.c',
    dependencies : [
        dependency('libobs'),
        dependency('wayland-client'),
    ],
)
