/* This file is part of RTags.

RTags is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTags is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#ifndef FollowLocationJob_h
#define FollowLocationJob_h

#include <rct/String.h>
#include <rct/List.h>
#include "RTags.h"
#include "QueryJob.h"
#include "Location.h"

class FollowLocationJob : public QueryJob
{
public:
    FollowLocationJob(const Location &loc, const std::shared_ptr<QueryMessage> &query, const std::shared_ptr<Project> &project);
protected:
    virtual int execute() override;
private:
    const Location location;
};

#endif
