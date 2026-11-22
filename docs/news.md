# Release notes for version 3.6 {#news}

[TOC]


## New features {#features}

#### Joystick state is updated before the connection callback {#joystick_connect_state_36}

When a joystick is connected, its axes, buttons and hats are now polled and its
gamepad mapping selected before the joystick callback is called, so input
functions like @ref glfwGetGamepadState return the current state of the device
both inside and outside the callback.  When a joystick is disconnected, its
state is cleared before the callback is called, so buttons held at
disconnection do not keep driving the application.

## Caveats {#caveats}

## Deprecations {#deprecations}

## Removals {#removals}

## New symbols {#new_symbols}

### New functions {#new_functions}

### New types {#new_types}

### New constants {#new_constants}

## Release notes for earlier versions {#news_archive}

- [Release notes for 3.5](https://www.glfw.org/docs/3.5/news.html)
- [Release notes for 3.4](https://www.glfw.org/docs/3.4/news.html)
- [Release notes for 3.3](https://www.glfw.org/docs/3.3/news.html)
- [Release notes for 3.2](https://www.glfw.org/docs/3.2/news.html)
- [Release notes for 3.1](https://www.glfw.org/docs/3.1/news.html)
- [Release notes for 3.0](https://www.glfw.org/docs/3.0/news.html)

