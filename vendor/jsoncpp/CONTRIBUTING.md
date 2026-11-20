# Contributing to JsonCpp

## Building

Both CMake and Meson tools are capable of generating a variety of build environments for you preferred development environment.
Using cmake or meson you can generate an XCode, Visual Studio, Unix Makefile, Ninja, or other environment that fits your needs.

An example of a common Meson/Ninja environment is described next.

## Building and testing with Meson/Ninja
Thanks to David Seifert (@SoapGentoo), we (the maintainers) now use
[meson](http://mesonbuild.com/) and [ninja](https://ninja-build.org/) to build
for debugging, as well as for continuous integration (see
[`./.travis_scripts/meson_builder.sh`](./.travis_scripts/meson_builder.sh) ). Other systems may work, but minor
things like version strings might break.

First, install both meson (which requires Python3) and ninja.
If you wish to install to a directory other than /usr/local, set an environment variable called DESTDIR with the desired path:
    DESTDIR=/path/to/install/dir

Then,
```sh
    cd jsoncpp/
    BUILD_TYPE=debug
    #BUILD_TYPE=release
    LIB_TYPE=shared
    #LIB_TYPE=static
    meson --buildtype ${BUILD_TYPE} --default-library ${LIB_TYPE} . build-${LIB_TYPE}
    ninja -v -C build-${LIB_TYPE}

    ninja -C build-static/ test

    # Or
    #cd build-${LIB_TYPE}
    #meson test --no-rebuild --print-errorlogs

    sudo ninja install
```

## Building and testing with other build systems
See https://github.com/open-source-parsers/jsoncpp/wiki/Building

## Tests

The HSVJEngine integration consumes JsonCpp as a library only. JsonCpp's
upstream test executables and fixtures are intentionally not part of this
vendor checkout; run the product checks from the repository root instead.

## Building the documentation

Run the Python script `doxybuild.py` from the top directory:

    python doxybuild.py --doxygen=$(which doxygen) --open --with-dot

See `doxybuild.py --help` for options.

## Versioning rules

Consumers of this library require a strict approach to incrementing versioning of the JsonCpp library. Currently, we follow the below set of rules:

* Any new public symbols require a minor version bump.
* Any alteration or removal of public symbols requires a major version bump, including changing the size of a class. This is necessary for
consumers to do dependency injection properly.

## Preparing code for submission

Generally, JsonCpp's style guide has been pretty relaxed, with the following common themes:

* Variables and function names use lower camel case (E.g. parseValue or collectComments).
* Class use camel case (e.g. OurReader)
* Member variables have a trailing underscore
* Prefer `nullptr` over `NULL`.
* Passing by non-const reference is allowed.
* Single statement if blocks may omit brackets.
* Generally prefer less space over more space.

For an example:

```c++
bool Reader::decodeNumber(Token& token) {
  Value decoded;
  if (!decodeNumber(token, decoded))
    return false;
  currentValue().swapPayload(decoded);
  currentValue().setOffsetStart(token.start_ - begin_);
  currentValue().setOffsetLimit(token.end_ - begin_);
  return true;
}
```

Before submitting your code, ensure that you meet the versioning requirements above, follow the style guide of the file you are modifying (or the above rules for new files), and run clang format. Meson exposes clang format with the following command:
```
ninja -v -C build-${LIB_TYPE}/ clang-format
```

For convenience, you can also run the `reformat.sh` script located in the root directory.
