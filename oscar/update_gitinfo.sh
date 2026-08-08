#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd $DIR

git rev-parse --git-dir &>/dev/null
if [ $? -eq 0 ]; then
    GIT_BRANCH=`git rev-parse --abbrev-ref HEAD`
    [ "$GIT_BRANCH" == "HEAD" ] && GIT_BRANCH=""  # not really a branch
    # GIT_BRANCH is embedded as semver build metadata (version.cpp), which per
    # https://semver.org/spec/v2.0.0.html only allows [0-9A-Za-z-] (dot-separated).
    # Branch names routinely contain "/" (e.g. "fix/foo"), which isn't in that set;
    # left unsanitized, that makes the resulting version string fail semver parsing
    # and the app refuses to start ("Version ... is invalid, cannot continue!").
    # Replace any disallowed character with "-" so the build metadata is always valid.
    GIT_BRANCH=`echo -n "$GIT_BRANCH" | tr -c 'A-Za-z0-9-' '-' | tr -s '-'`
    GIT_REVISION=`git rev-parse --short HEAD`
    $(git diff-index --quiet HEAD --)
    if [ $? -ne 0 ]; then
        GIT_REVISION="${GIT_REVISION}-plus"  # uncommitted changes
    else
        # only use the tag if clean
        GIT_TAG=`git describe --exact-match --tags 2>/dev/null`
    fi
fi

echo // This is an auto generated file > $DIR/git_info.h.new
[ -n "$GIT_BRANCH" ] && echo "#define GIT_BRANCH \"$GIT_BRANCH\"" >> $DIR/git_info.h.new
[ -n "$GIT_REVISION" ] && echo "#define GIT_REVISION \"$GIT_REVISION\"" >> $DIR/git_info.h.new
[ -n "$GIT_TAG" ] && echo "#define GIT_TAG \"$GIT_TAG\"" >> $DIR/git_info.h.new

if diff $DIR/git_info.h $DIR/git_info.h.new &> /dev/null; then
    rm $DIR/git_info.h.new
else
    echo Updating $DIR/git_info.h
    mv $DIR/git_info.h.new $DIR/git_info.h
fi
