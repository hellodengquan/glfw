//========================================================================
// Preview window picking and hover ownership test
// Copyright (c) Camilla Löwy <elmindreda@glfw.org>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================
//
// This test models an editor or game with two preview panes and checks, using
// only the public GLFW window, focus and cursor semantics, that
//
//  - the scene picking target is the preview whose content area the pointer
//    is actually over, never the preview that has input focus;
//  - the cursor position polled through a preview, translated by that
//    preview's own content area position, describes the pointer location even
//    when that preview is not hovered, not focused or hidden;
//  - a relative pointer displacement has the same screen-space meaning when
//    polled through either preview, i.e. each preview keeps its own frame;
//  - every cursor enter notification that is delivered belongs to the content
//    area the pointer actually entered, including while another preview has
//    input focus;
//  - hiding a preview under the pointer withdraws hover and picking from it,
//    and re-showing it under the pointer returns them;
//  - showing or focusing a preview the pointer is not over never fabricates
//    hover for it nor misattributes a notification to it.
//
// The pointer is moved with glfwSetCursorPos on the currently focused preview,
// which is the only portable way to move the pointer through the GLFW API.
// The ground truth for where the pointer is comes from polling glfwGetCursorPos
// and translating it by the content area position of the same window, since
// the documentation defines that position for any window.
//
// Two modes are provided:
//
//  - the default, unattended mode moves the pointer from code and checks the
//    state that must follow it on any platform;
//  - the pointer-led mode (-p) asks the operator to move the physical mouse.
//    It is the only way to produce real pointer motion on platforms where a
//    warped cursor generates no motion, and it exercises real enter/leave
//    delivery everywhere.
//
// Platform policy.  The documentation promises enter/leave notification when
// the cursor enters or leaves a window's content area, but it does not promise
// that programmatically moving the pointer synthesizes pointer motion, nor
// that windows without input focus are tracked.  For example, on macOS a
// warped cursor produces no mouse-moved events at all, so tracking areas stay
// silent, whereas X11 warps do generate crossing events.  The test therefore
// distinguishes three kinds of checks:
//
//  - delivered-notification checks depend on whether the window system reports
//    the pointer motion the test performed; they are probed once at start and
//    count as failures only when motion reporting was observed;
//  - ownership checks judge the final enter/leave classification against where
//    the pointer settled, and fail whenever a preview ends up claiming a
//    pointer that is not over its content area;
//  - per-window state checks (GLFW_HOVERED and polled positions) never depend
//    on notification delivery and always count as failures.
//
// The preview windows must be visible and unobstructed; run this test on an
// interactive desktop session.  Platforms that do not expose window positions
// or cursor warping (Wayland, Null) are skipped.
//
// Exit status: 0 if all checks passed, 1 if any check failed, 77 if skipped.
//
//========================================================================

#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define EXIT_SKIP 77

#define PREVIEW_COUNT   2
#define PREVIEW_WIDTH   240
#define PREVIEW_HEIGHT  180
#define PREVIEW_GAP     96
#define WORKAREA_MARGIN 48

// Allowance for the 1:1 mapping between content area relative cursor
// coordinates and screen coordinates, e.g. the macOS base 0,1 note
#define COORD_TOLERANCE 2.0

// Allowance around a content area when deciding which preview a notification
// belongs to, covering window shadows and borders.  It is smaller than half
// the gap between the previews, so a notification naming the wrong preview
// can never fall inside this allowance by accident.
#define OWNERSHIP_MARGIN 16.0

// Time given to the window system to deliver events after a change
#define SETTLE_SECONDS  0.35

// A preview pane and the state collected for it
//
typedef struct Preview
{
    GLFWwindow* window;
    const char* title;
    int index;

    // Last enter/leave classification delivered to this preview; it describes
    // whether the cursor was inside its content area as far as the library was
    // told
    int entered;

    // Pointer location sampled at the last enter/leave transition, used only
    // to explain a verdict about notification ownership
    double transition_x;
    double transition_y;

    // Per step notification counts, used to check delivery and attribution
    int step_enters;
    int step_leaves;
    int step_positions;

    // Last cursor position reported through the position callback, relative to
    // this preview's own content area
    double reported_x;
    double reported_y;
    int reported;
} Preview;

static Preview previews[PREVIEW_COUNT];

// Nonzero on platforms that deliver mouse tracking to windows without input
// focus
static int tracks_unfocused_windows;

// Nonzero when moving the pointer was observed to deliver enter notifications
// and cursor position callbacks to the focused preview
static int delivers_notifications;

// Pointer location in screen coordinates, derived by polling a window and
// translating the result by that window's content area position
static double pointer_x;
static double pointer_y;

// Number of cursor position notifications delivered so far, used by the
// pointer-led mode to tell real pointer motion from a step without any
static int motion_notifications;

// Nonzero while checks are recorded; cleared while probing and restoring
static int testing = 1;

static int check_count;
static int failure_count;
static int warning_count;

//========================================================================
// Reporting
//========================================================================

static void error_callback(int error, const char* description)
{
    fprintf(stderr, "Error: %s\n", description);
}

// Records one check result
//
// Hard checks describe behavior promised by the documentation and count as
// failures.  Soft checks describe behavior that depends on the window system
// reporting the pointer motion performed by the test or tracking an unfocused
// window; they are reported without failing the run.
//
// Messages are written as statements of fact that read correctly whether they
// pass or fail, so the values that led to the verdict are always visible.
//
static void check(int condition, int hard, const char* format, ...)
{
    va_list args;

    // Probing and restoring change the pointer on purpose; their effects are
    // not verdicts
    if (!testing)
        return;

    check_count++;

    if (condition)
        printf("ok    ");
    else if (hard)
    {
        failure_count++;
        printf("FAIL  ");
    }
    else
    {
        warning_count++;
        printf("WARN  ");
    }

    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

static void note(const char* format, ...)
{
    va_list args;

    if (!testing)
        return;

    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

static double magnitude(double value)
{
    return value < 0.0 ? -value : value;
}

static int close_enough(double first, double second)
{
    return magnitude(first - second) <= COORD_TOLERANCE;
}

static int tracks(void)
{
    return tracks_unfocused_windows;
}

static int delivers(void)
{
    return delivers_notifications;
}

//========================================================================
// Geometry and pointer helpers
//========================================================================

static void get_content_rect(Preview* preview, int* xpos, int* ypos,
                             int* width, int* height)
{
    glfwGetWindowPos(preview->window, xpos, ypos);
    glfwGetWindowSize(preview->window, width, height);
}

// Translates a content area relative cursor position into screen coordinates
//
static void to_screen(Preview* preview, double xpos, double ypos,
                      double* screen_x, double* screen_y)
{
    int window_x, window_y;

    glfwGetWindowPos(preview->window, &window_x, &window_y);

    *screen_x = window_x + xpos;
    *screen_y = window_y + ypos;
}

// Polls the pointer position through a window and translates it to screen
// coordinates; this uses only state the documentation defines per window
//
static void poll_pointer(Preview* preview, double* screen_x, double* screen_y)
{
    double xpos, ypos;

    glfwGetCursorPos(preview->window, &xpos, &ypos);
    to_screen(preview, xpos, ypos, screen_x, screen_y);
}

// Whether a screen point lies within a content area, optionally enlarged
//
static int point_in_preview(Preview* preview, double screen_x, double screen_y,
                            double margin)
{
    int xpos, ypos, width, height;

    if (!glfwGetWindowAttrib(preview->window, GLFW_VISIBLE))
        return 0;

    get_content_rect(preview, &xpos, &ypos, &width, &height);

    return screen_x >= xpos - margin &&
           screen_x < xpos + width + margin &&
           screen_y >= ypos - margin &&
           screen_y < ypos + height + margin;
}

// Determines which preview the pointer is over from the content area
// rectangle of every visible preview, i.e. the preview a scene renderer
// would be picking in at that pointer location
//
static int geometrical_pick(double screen_x, double screen_y)
{
    int i;

    for (i = 0;  i < PREVIEW_COUNT;  i++)
    {
        if (point_in_preview(previews + i, screen_x, screen_y, 0.0))
            return i;
    }

    return -1;
}

static Preview* find_focused(void)
{
    int i;

    for (i = 0;  i < PREVIEW_COUNT;  i++)
    {
        if (glfwGetWindowAttrib(previews[i].window, GLFW_FOCUSED))
            return previews + i;
    }

    return NULL;
}

static Preview* find_visible(void)
{
    int i;

    for (i = 0;  i < PREVIEW_COUNT;  i++)
    {
        if (glfwGetWindowAttrib(previews[i].window, GLFW_VISIBLE))
            return previews + i;
    }

    return NULL;
}

// Updates the ground truth pointer location, preferring the focused preview
// because that is the window the pointer was moved through
//
static int update_pointer(void)
{
    Preview* source = find_focused();

    if (!source)
        source = find_visible();
    if (!source)
        return 0;

    poll_pointer(source, &pointer_x, &pointer_y);
    return 1;
}

//========================================================================
// Rendering and event processing
//========================================================================

static void paint_all(void)
{
    int i;

    for (i = 0;  i < PREVIEW_COUNT;  i++)
    {
        Preview* preview = previews + i;

        if (!glfwGetWindowAttrib(preview->window, GLFW_VISIBLE))
            continue;

        glfwMakeContextCurrent(preview->window);
        glClearColor(preview->index ? 0.25f : 0.65f, 0.3f, 0.45f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(preview->window);
    }
}

// Waits for the window system to deliver the events caused by a change
//
static void settle(void)
{
    const double deadline = glfwGetTime() + SETTLE_SECONDS;

    do
    {
        glfwWaitEventsTimeout(0.02);
        paint_all();
    }
    while (glfwGetTime() < deadline);
}

//========================================================================
// Callbacks
//========================================================================

static void cursor_enter_callback(GLFWwindow* window, int entered)
{
    Preview* preview = glfwGetWindowUserPointer(window);

    // Notifications are only recorded here, not judged.  While the pointer is
    // moving, position events and enter/leave events interleave, so a pointer
    // sampled inside a callback may already have moved on; ownership is judged
    // once the pointer has settled, in verify_state.  What is recorded is the
    // pointer location at the moment of the transition, used only to explain
    // the final verdict.
    poll_pointer(preview, &preview->transition_x, &preview->transition_y);

    if (entered)
    {
        if (preview->entered == entered)
        {
            // Some platforms deliver redundant transitions around window
            // borders and shadows (see glfw/glfw#1888)
            note("%s: note repeated enter notification without an intervening "
                 "leave", preview->title);
        }

        preview->step_enters++;
    }
    else
        preview->step_leaves++;

    preview->entered = entered;
}

static void cursor_position_callback(GLFWwindow* window, double xpos, double ypos)
{
    Preview* preview = glfwGetWindowUserPointer(window);

    preview->reported_x = xpos;
    preview->reported_y = ypos;
    preview->reported = 1;
    preview->step_positions++;
    motion_notifications++;
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (key != GLFW_KEY_ESCAPE || action != GLFW_PRESS)
        return;

    glfwSetWindowShouldClose(window, GLFW_TRUE);
}

//========================================================================
// Steps
//========================================================================

static int wait_mode;

// Nonzero when the operator moves the pointer instead of the test warping it
static int pointer_led;

static void reset_step_counts(void)
{
    int i;

    for (i = 0;  i < PREVIEW_COUNT;  i++)
    {
        previews[i].step_enters = 0;
        previews[i].step_leaves = 0;
        previews[i].step_positions = 0;
    }
}

static void begin_step(const char* description)
{
    int i;

    printf("\n== %s\n", description);
    fflush(stdout);

    update_pointer();
    reset_step_counts();

    // The transition sample explains the verdict on enter state; start it at
    // the current pointer so it never reads as if sampled at an origin
    for (i = 0;  i < PREVIEW_COUNT;  i++)
    {
        previews[i].transition_x = pointer_x;
        previews[i].transition_y = pointer_y;
    }

    if (wait_mode)
    {
        printf("(press Enter to continue)\n");
        fflush(stdout);
        while (getchar() != '\n' && !feof(stdin))
            ;
    }
}

// Moves the pointer to a screen coordinate through the focused preview and
// waits for the window system to catch up
//
static int move_pointer(double screen_x, double screen_y)
{
    Preview* driver = find_focused();
    int window_x, window_y;

    if (!driver)
    {
        check(0, 1, "cannot move the pointer: no preview has input focus");
        return 0;
    }

    glfwGetWindowPos(driver->window, &window_x, &window_y);
    glfwSetCursorPos(driver->window, screen_x - window_x, screen_y - window_y);

    settle();

    if (!update_pointer())
    {
        check(0, 1, "cannot determine the pointer position after moving it");
        return 0;
    }

    return 1;
}

static int acquire_focus(Preview* preview)
{
    int i;

    glfwFocusWindow(preview->window);

    for (i = 0;  i < 20;  i++)
    {
        if (glfwGetWindowAttrib(preview->window, GLFW_FOCUSED))
            return 1;

        glfwWaitEventsTimeout(0.05);
        paint_all();
    }

    return glfwGetWindowAttrib(preview->window, GLFW_FOCUSED);
}

static void centre_of(Preview* preview, double* screen_x, double* screen_y)
{
    int xpos, ypos, width, height;

    get_content_rect(preview, &xpos, &ypos, &width, &height);
    *screen_x = xpos + width / 2.0;
    *screen_y = ypos + height / 2.0;
}

// Checks that hover, picking, per-window cursor frames and delivered enter
// state of both previews belong to the content area the pointer is over
//
static void verify_state(void)
{
    int pick, i;

    update_pointer();
    pick = geometrical_pick(pointer_x, pointer_y);

    printf("  pointer at screen (%0.1f, %0.1f); content area under pointer: %s\n",
           pointer_x, pointer_y,
           pick < 0 ? "none" : previews[pick].title);

    for (i = 0;  i < PREVIEW_COUNT;  i++)
    {
        Preview* preview = previews + i;

        const int focused = glfwGetWindowAttrib(preview->window, GLFW_FOCUSED);
        const int visible = glfwGetWindowAttrib(preview->window, GLFW_VISIBLE);
        const int hovered = glfwGetWindowAttrib(preview->window, GLFW_HOVERED);
        const int expected = (pick == i);

        // GLFW_HOVERED describes the pointer being over the content area with
        // nothing between, so it must follow the pointer, never the focus
        check(hovered == expected, 1,
              "%s: GLFW_HOVERED is %s while the pointer is %s its content area",
              preview->title, hovered ? "true" : "false",
              expected ? "over" : "not over");

        // glfwGetCursorPos is defined for any window, so the polling result of
        // every preview must describe the same pointer, whether that preview
        // is hovered, focused, hidden or not
        {
            double screen_x, screen_y;

            poll_pointer(preview, &screen_x, &screen_y);
            check(close_enough(screen_x, pointer_x) &&
                  close_enough(screen_y, pointer_y), 1,
                  "%s: polled cursor position maps to screen (%0.1f, %0.1f) "
                  "while the pointer is at (%0.1f, %0.1f)",
                  preview->title, screen_x, screen_y, pointer_x, pointer_y);
        }

        // A visible preview claiming the pointer while the pointer is outside
        // its content area would misattribute picking input, so that is always
        // a defect.  A preview not claiming a pointer that is over it, or a
        // hidden preview that never got a matching leave, is a delivery-policy
        // matter instead: it is only a defect where delivery is expected.
        if (visible && preview->entered && !hovered)
        {
            check(0, 1,
                  "%s: last enter/leave notification was entered=true while "
                  "the pointer at (%0.1f, %0.1f) is not over its content area",
                  preview->title, pointer_x, pointer_y);
        }
        else
        {
            check(preview->entered == hovered,
                  delivers() && (focused || tracks()),
                  "%s: last enter/leave notification was entered=%s while the "
                  "pointer at (%0.1f, %0.1f) is %s the content area (last "
                  "transition sampled at (%0.1f, %0.1f); window visible: %s, "
                  "has input focus: %s)",
                  preview->title, preview->entered ? "true" : "false",
                  pointer_x, pointer_y,
                  hovered ? "over" : "outside",
                  preview->transition_x, preview->transition_y,
                  visible ? "yes" : "no", focused ? "yes" : "no");
        }

        // A position delivered by the callback is documented to be in this
        // preview's own frame, so translating it by this preview's content
        // area position must land on the pointer
        if (preview->reported)
        {
            double reported_x, reported_y;

            to_screen(preview, preview->reported_x, preview->reported_y,
                      &reported_x, &reported_y);
            check(close_enough(reported_x, pointer_x) &&
                  close_enough(reported_y, pointer_y), 1,
                  "%s: reported cursor position (%0.1f, %0.1f) maps to screen "
                  "(%0.1f, %0.1f) while the pointer is at (%0.1f, %0.1f)",
                  preview->title, preview->reported_x, preview->reported_y,
                  reported_x, reported_y, pointer_x, pointer_y);
        }
    }
}

// Checks how many notifications a preview received during the step
//
static void verify_counts(Preview* preview, int enters, int leaves,
                          const char* description)
{
    const int tracked = delivers() &&
                        (glfwGetWindowAttrib(preview->window, GLFW_FOCUSED) ||
                         tracks());

    check(preview->step_enters == enters && preview->step_leaves == leaves,
          tracked,
          "%s: %s (enter %i, leave %i during the step; motion reporting "
          "observed: %s)",
          preview->title, description,
          preview->step_enters, preview->step_leaves,
          delivers() ? "yes" : "no");
}

// Checks that a preview received no notification at all during the step
//
static void verify_untouched(Preview* preview, const char* reason)
{
    check(preview->step_enters == 0 && preview->step_leaves == 0, 1,
          "%s: enter %i, leave %i while %s",
          preview->title, preview->step_enters, preview->step_leaves, reason);
}

// Checks that a preview owning the pointer reported the pointer position in
// its own content area frame during the step
//
static void verify_delivered_position(Preview* preview)
{
    check(preview->step_positions > 0, delivers(),
          "%s: reported cursor position %i time(s) while the pointer moved "
          "inside its content area (motion reporting observed: %s)",
          preview->title, preview->step_positions,
          delivers() ? "yes" : "no");
}

// Polls the cursor frame of every preview for relative displacement checks
//
static void poll_frames(double frames[PREVIEW_COUNT][2])
{
    int i;

    for (i = 0;  i < PREVIEW_COUNT;  i++)
        glfwGetCursorPos(previews[i].window, &frames[i][0], &frames[i][1]);
}

// What the pointer-led mode asks the operator to move the pointer onto
//
enum
{
    TARGET_PREVIEW,
    TARGET_GAP
};

// In pointer-led mode, waits for the operator to move the physical mouse until
// the pointer is over the requested content area (or between the previews) and
// the window system has reported the motion.  This is the only way to make
// real pointer motion happen on platforms where a warped cursor produces no
// motion at all.  Returns nonzero when the requested target was reached, so
// that the checks which depend on it can be skipped rather than misreported
// when the operator aims elsewhere.
//
static int wait_for_target(int kind, int index, const char* instruction)
{
    const int before = motion_notifications;
    const char* timeout = getenv("GLFW_TEST_PICKING_TIMEOUT");
    const double deadline = glfwGetTime() +
                            (timeout ? atof(timeout) : 45.0);
    int reached = 0;

    printf("  %s\n", instruction);
    fflush(stdout);

    while (glfwGetTime() < deadline)
    {
        glfwWaitEventsTimeout(0.05);
        paint_all();
        update_pointer();

        if (motion_notifications == before)
            continue;

        if (kind == TARGET_PREVIEW && geometrical_pick(pointer_x, pointer_y) == index)
        {
            reached = 1;
            break;
        }
        else if (kind == TARGET_GAP && geometrical_pick(pointer_x, pointer_y) < 0)
        {
            reached = 1;
            break;
        }
    }

    settle();
    update_pointer();

    check(motion_notifications > before, 0,
          "the window system reported pointer motion while the operator moved "
          "the mouse (%i notification(s))", motion_notifications - before);
    check(reached, 0,
          "the operator moved the pointer onto the requested target: %s",
          reached ? "yes" : "no");

    return reached;
}

// Pointer-led scenario: the operator moves the physical mouse, so that
// enter/leave notifications are generated by real pointer motion on every
// platform.  Every check is guarded by whether the preceding instruction was
// actually followed, so a missed target is reported once instead of producing
// a cascade of misleading failures.
//
static void run_pointer_led(void)
{
    //--------------------------------------------------------------------
    // Real motion carries the pointer into the focused preview
    //--------------------------------------------------------------------

    check(acquire_focus(previews + 0), 0,
          "Preview 1: acquires input focus");
    begin_step("the pointer is moved into the focused preview");

    if (wait_for_target(TARGET_PREVIEW, 0, "move the mouse into Preview 1"))
    {
        verify_state();

        check(previews[0].entered == 1, 1,
              "Preview 1: last enter/leave notification was entered=%s while "
              "the pointer is inside its content area",
              previews[0].entered ? "true" : "false");
        check(previews[0].step_enters >= 1, 1,
              "Preview 1: reported %i enter notification(s) for the real "
              "crossing", previews[0].step_enters);
        // Any notification the operator's path produced for Preview 2 is only
        // a defect if it left Preview 2 claiming a pointer that is elsewhere;
        // verify_state judges that final classification for both previews
        note("Preview 2 received %i enter and %i leave notification(s) while "
             "the pointer was moved into Preview 1 (the operator's path may "
             "have crossed it)",
             previews[1].step_enters, previews[1].step_leaves);

        //----------------------------------------------------------------
        // Focus moves to the other preview while the pointer stays put
        //----------------------------------------------------------------

        begin_step("input focus moves to the other preview while the pointer "
                   "stays inside the first");
        check(acquire_focus(previews + 1), 0,
              "Preview 2: acquires input focus");
        settle();
        verify_state();

        check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED), 1,
              "Preview 1: remains hovered while the other preview has focus");
        check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED) == 0, 1,
              "Preview 2: is not hovered merely because it has input focus");
        check(geometrical_pick(pointer_x, pointer_y) == 0, 1,
              "the pointer still picks Preview 1, not the focused preview");

        //----------------------------------------------------------------
        // Relative displacement inside the hovered preview, reported in each
        // preview's own frame
        //----------------------------------------------------------------

        begin_step("the pointer moves a little inside the hovered preview");
        {
            double before[PREVIEW_COUNT][2];
            double after[PREVIEW_COUNT][2];

            reset_step_counts();
            poll_frames(before);

            if (wait_for_target(TARGET_PREVIEW, 0,
                                "move the mouse a little, staying inside "
                                "Preview 1"))
            {
                poll_frames(after);
                verify_state();

                {
                    const double delta_0_x = after[0][0] - before[0][0];
                    const double delta_0_y = after[0][1] - before[0][1];
                    const double delta_1_x = after[1][0] - before[1][0];
                    const double delta_1_y = after[1][1] - before[1][1];

                    check(close_enough(delta_0_x, delta_1_x) &&
                          close_enough(delta_0_y, delta_1_y), 1,
                          "both previews report the same screen-space "
                          "displacement (%0.1f, %0.1f) vs (%0.1f, %0.1f)",
                          delta_0_x, delta_0_y, delta_1_x, delta_1_y);
                    check(magnitude(delta_0_x) > 1.0 ||
                          magnitude(delta_0_y) > 1.0, 0,
                          "the pointer actually moved during the step");
                }

                check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED), 1,
                      "Preview 1: stays the picking target");
                check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED) == 0,
                      1, "Preview 2: is not the picking target");
            }
        }

        //----------------------------------------------------------------
        // Real motion carries the pointer into the focused preview
        //----------------------------------------------------------------

        begin_step("the pointer crosses into the preview that has input focus");

        if (wait_for_target(TARGET_PREVIEW, 1, "move the mouse into Preview 2"))
        {
            verify_state();

            check(previews[1].entered == 1, 1,
                  "Preview 2: last enter/leave notification was entered=%s "
                  "while the pointer is inside its focused content area",
                  previews[1].entered ? "true" : "false");
            check(previews[1].step_enters >= 1, 1,
                  "Preview 2: reported %i enter notification(s) for the real "
                  "crossing", previews[1].step_enters);
            check(previews[0].entered == 0, 1,
                  "Preview 1: last enter/leave notification was entered=%s "
                  "after the pointer left its content area",
                  previews[0].entered ? "true" : "false");
            check(previews[0].step_leaves >= 1, 1,
                  "Preview 1: reported %i leave notification(s) for the real "
                  "crossing", previews[0].step_leaves);
            check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
                  "Preview 2: is the picking target");
            check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED) == 0, 1,
                  "Preview 1: is not the picking target");

            //------------------------------------------------------------
            // The preview under the pointer is hidden and shown again
            //------------------------------------------------------------

            begin_step("the hovered preview is hidden under the pointer");
            glfwHideWindow(previews[1].window);
            settle();
            verify_state();

            check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED) == 0, 1,
                  "Preview 2: does not stay hovered after being hidden under "
                  "the pointer");
            check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED) == 0, 1,
                  "Preview 1: does not gain hover when the preview under the "
                  "pointer is hidden");
            check(geometrical_pick(pointer_x, pointer_y) < 0, 1,
                  "the pointer no longer picks either preview");
            check(previews[1].entered == 0, delivers(),
                  "Preview 2: last enter/leave notification was entered=%s "
                  "after being hidden under the pointer",
                  previews[1].entered ? "true" : "false");

            begin_step("the hidden preview is shown again under the pointer");
            glfwShowWindow(previews[1].window);
            settle();
            verify_state();

            check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
                  "Preview 2: is hovered again because the pointer is over its "
                  "content area");
            check(geometrical_pick(pointer_x, pointer_y) == 1, 1,
                  "the pointer picks the re-shown preview");

            //------------------------------------------------------------
            // Focus moves to the preview not under the pointer
            //------------------------------------------------------------

            begin_step("input focus moves to the preview that is not hovered");
            check(acquire_focus(previews + 0), 0,
                  "Preview 1: acquires input focus");
            settle();
            verify_state();

            check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
                  "Preview 2: remains hovered and the picking target");
            check(geometrical_pick(pointer_x, pointer_y) == 1, 1,
                  "the pointer still picks the preview under it, not the "
                  "focused one");
            check(previews[0].entered == 0, 1,
                  "Preview 1: last enter/leave notification was entered=%s "
                  "while the pointer is not over its content area",
                  previews[0].entered ? "true" : "false");
        }
    }
}

//========================================================================
// Setup
//========================================================================

static void usage(void)
{
    printf("Usage: picking [-h] [-w] [-p]\n");
    printf("Options:\n");
    printf("  -h show this help\n");
    printf("  -p pointer-led mode: move the physical mouse when prompted; use\n"
           "     this to exercise real enter/leave delivery on platforms where\n"
           "     moving the pointer from code produces no motion events\n");
    printf("  -w wait for Enter after each step\n");
}

static int create_preview(Preview* preview, const char* title, int index)
{
    preview->title = title;
    preview->index = index;

    preview->window = glfwCreateWindow(PREVIEW_WIDTH, PREVIEW_HEIGHT,
                                       title, NULL, NULL);
    if (!preview->window)
        return 0;

    glfwSetWindowUserPointer(preview->window, preview);
    glfwSetCursorEnterCallback(preview->window, cursor_enter_callback);
    glfwSetCursorPosCallback(preview->window, cursor_position_callback);
    glfwSetKeyCallback(preview->window, key_callback);

    glfwMakeContextCurrent(preview->window);
    gladLoadGL(glfwGetProcAddress);
    glfwSwapInterval(0);

    return 1;
}

// Places the previews side by side with a gap between their content areas so
// the pointer can be over neither of them
//
static int place_previews(double* gap_x, double* gap_y)
{
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    int work_x, work_y, work_width, work_height;
    int left_x, row_y, right_x;
    int xpos, ypos, width, height;
    int left_edge, right_edge;

    if (!monitor)
    {
        printf("SKIP: no monitor available\n");
        return 0;
    }

    glfwGetMonitorWorkarea(monitor, &work_x, &work_y,
                           &work_width, &work_height);

    if (work_width < WORKAREA_MARGIN * 2 + PREVIEW_WIDTH * 2 + PREVIEW_GAP ||
        work_height < WORKAREA_MARGIN * 2 + PREVIEW_HEIGHT)
    {
        printf("SKIP: the monitor work area is too small for the previews\n");
        return 0;
    }

    left_x = work_x + WORKAREA_MARGIN +
             (work_width - WORKAREA_MARGIN * 2 -
              PREVIEW_WIDTH * 2 - PREVIEW_GAP) / 2;
    row_y = work_y + WORKAREA_MARGIN +
            (work_height - WORKAREA_MARGIN * 2 - PREVIEW_HEIGHT) / 3;
    right_x = left_x + PREVIEW_WIDTH + PREVIEW_GAP;

    glfwSetWindowPos(previews[0].window, left_x, row_y);
    glfwSetWindowPos(previews[1].window, right_x, row_y);

    glfwShowWindow(previews[0].window);
    glfwShowWindow(previews[1].window);
    settle();

    get_content_rect(previews + 0, &xpos, &ypos, &width, &height);
    left_edge = xpos + width;
    *gap_y = ypos + height / 2.0;

    get_content_rect(previews + 1, &xpos, &ypos, &width, &height);
    right_edge = xpos;

    if (left_edge + 2 * OWNERSHIP_MARGIN > right_edge)
    {
        printf("SKIP: the window system placed the previews too close "
               "together (content areas meet at %i and %i, gap %i)\n",
               left_edge, right_edge, right_edge - left_edge);
        return 0;
    }

    *gap_x = (left_edge + right_edge) / 2.0;
    return 1;
}

// Determines whether the window system reports the pointer motion performed by
// glfwSetCursorPos, by moving the pointer into the focused preview and looking
// for the matching notifications.  Platforms disagree here: X11 warps generate
// crossing events, while macOS warps generate none.  The probe itself records
// no checks; it only sets the policy for the checks that follow.
//
static void probe_delivery(double gap_x, double gap_y)
{
    int i;
    double centre_x, centre_y;

    testing = 0;

    reset_step_counts();
    for (i = 0;  i < PREVIEW_COUNT;  i++)
    {
        previews[i].entered = 0;
        previews[i].reported = 0;
    }

    move_pointer(gap_x, gap_y);
    reset_step_counts();

    centre_of(previews + 0, &centre_x, &centre_y);
    move_pointer(centre_x, centre_y);

    delivers_notifications =
        previews[0].step_enters > 0 || previews[0].step_positions > 0;

    note("moving the pointer into the focused preview produced %i enter and "
         "%i cursor position notification(s); notifications %s counted as "
         "delivered",
         previews[0].step_enters, previews[0].step_positions,
         delivers_notifications ? "are" : "are not");

    // End the probe with the pointer outside both previews
    move_pointer(gap_x, gap_y);
    reset_step_counts();
    for (i = 0;  i < PREVIEW_COUNT;  i++)
        previews[i].entered = 0;

    testing = 1;
}

// Warp-driven scenario: the test moves the pointer with glfwSetCursorPos and
// checks the per-window state that never depends on notification delivery,
// plus the attribution of any notification the platform chooses to deliver
// for the warped movement
//
static void run_warped(double gap_x, double gap_y)
{
    int i;

    // Establish the baseline with the pointer between the previews, never over
    // either content area
    begin_step("baseline: pointer between the previews, first preview focused");
    move_pointer(gap_x, gap_y);
    verify_state();

    check(previews[0].entered == 0, 1,
          "Preview 1: last enter/leave notification was entered=%s while the "
          "pointer is between the previews",
          previews[0].entered ? "true" : "false");
    check(previews[1].entered == 0, 1,
          "Preview 2: last enter/leave notification was entered=%s while the "
          "pointer is between the previews",
          previews[1].entered ? "true" : "false");
    verify_untouched(previews + 1, "the pointer stayed outside its content "
                     "area");

    //--------------------------------------------------------------------
    // The pointer enters the content area of the focused preview
    //--------------------------------------------------------------------

    begin_step("pointer enters the content area of the focused preview");
    {
        double centre_x, centre_y;
        centre_of(previews + 0, &centre_x, &centre_y);
        move_pointer(centre_x, centre_y);
        verify_state();
    }

    check(glfwGetWindowAttrib(previews[0].window, GLFW_FOCUSED), 1,
          "Preview 1: the preview the pointer was moved through has input "
          "focus");
    check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED), 1,
          "Preview 1: is hovered after the pointer entered its content area");
    verify_counts(previews + 0, 1, 0,
                  "entering its content area produced one enter and no leave");
    verify_delivered_position(previews + 0);
    verify_untouched(previews + 1, "the pointer stayed outside its content "
                     "area");

    //--------------------------------------------------------------------
    // Input focus moves to the other preview while the pointer stays put
    //--------------------------------------------------------------------

    begin_step("input focus moves to the other preview while the pointer stays "
               "over the first");
    check(acquire_focus(previews + 1), 1,
          "Preview 2: acquires input focus");
    settle();
    verify_state();

    check(glfwGetWindowAttrib(previews[1].window, GLFW_FOCUSED), 1,
          "Preview 2: has input focus");
    check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED), 1,
          "Preview 1: remains hovered while the other preview has focus");
    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED) == 0, 1,
          "Preview 2: is not hovered merely because it has input focus");
    verify_counts(previews + 0, 0, 0,
                  "kept the pointer while focus moved away from it");
    verify_untouched(previews + 1, "the pointer stayed outside its content "
                     "area");

    //--------------------------------------------------------------------
    // Relative pointer displacement inside the hovered preview while the
    // other preview has focus: both windows must keep their own frame
    //--------------------------------------------------------------------

    begin_step("pointer moves inside the hovered preview while the other "
               "preview has focus");
    {
        int xpos, ypos, width, height;
        double before[PREVIEW_COUNT][2];
        double after[PREVIEW_COUNT][2];
        const double delta_x = 40.0;
        const double delta_y = 15.0;

        get_content_rect(previews + 0, &xpos, &ypos, &width, &height);

        // Start well inside the hovered preview so the displacement stays in
        // its content area
        move_pointer(xpos + width * 0.35, ypos + height * 0.4);
        verify_state();
        poll_frames(before);

        move_pointer(xpos + width * 0.35 + delta_x, ypos + height * 0.4 + delta_y);
        verify_state();
        poll_frames(after);

        for (i = 0;  i < PREVIEW_COUNT;  i++)
        {
            check(close_enough(after[i][0] - before[i][0], delta_x) &&
                  close_enough(after[i][1] - before[i][1], delta_y), 1,
                  "%s: relative displacement polled through it was "
                  "(%0.1f, %0.1f) while the pointer moved by (%0.1f, %0.1f)",
                  previews[i].title,
                  after[i][0] - before[i][0],
                  after[i][1] - before[i][1],
                  delta_x, delta_y);
        }

        check(glfwGetWindowAttrib(previews[1].window, GLFW_FOCUSED), 1,
              "Preview 2: still has input focus while the pointer moved inside "
              "the hovered preview");
    }

    check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED), 1,
          "Preview 1: stays the picking target while the pointer moved inside "
          "it");
    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED) == 0, 1,
          "Preview 2: is not the picking target when focus is there but the "
          "pointer is not");
    verify_counts(previews + 0, 0, 0,
                  "stayed entered while the pointer moved inside its content "
                  "area");
    verify_untouched(previews + 1, "the pointer stayed outside its content "
                     "area");

    //--------------------------------------------------------------------
    // The pointer crosses into the focused preview
    //--------------------------------------------------------------------

    begin_step("pointer crosses into the preview that has input focus");
    {
        double centre_x, centre_y;
        centre_of(previews + 1, &centre_x, &centre_y);
        move_pointer(centre_x, centre_y);
        verify_state();
    }

    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
          "Preview 2: is hovered after the pointer crossed into its content "
          "area");
    verify_counts(previews + 1, 1, 0,
                  "entering its content area produced one enter and no leave");
    verify_delivered_position(previews + 1);
    check(previews[0].entered == 0, delivers() && tracks(),
          "Preview 1: last enter/leave notification was entered=%s after the "
          "pointer left its content area (unfocused windows tracked: %s)",
          previews[0].entered ? "true" : "false", tracks() ? "yes" : "no");

    //--------------------------------------------------------------------
    // The preview under the pointer is hidden
    //--------------------------------------------------------------------

    begin_step("the hovered preview is hidden under the pointer");
    glfwHideWindow(previews[1].window);
    settle();
    verify_state();

    check(glfwGetWindowAttrib(previews[1].window, GLFW_VISIBLE) == 0, 1,
          "Preview 2: is not visible after being hidden");
    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED) == 0, 1,
          "Preview 2: does not stay hovered after being hidden under the "
          "pointer");
    check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED) == 0, 1,
          "Preview 1: does not gain hover when the preview under the pointer "
          "is hidden");
    check(geometrical_pick(pointer_x, pointer_y) < 0, 1,
          "the pointer no longer picks either preview");
    check(previews[1].entered == 0, delivers(),
          "Preview 2: last enter/leave notification was entered=%s after being "
          "hidden under the pointer",
          previews[1].entered ? "true" : "false");

    //--------------------------------------------------------------------
    // The hidden preview is shown again under the pointer
    //--------------------------------------------------------------------

    begin_step("the hidden preview is shown again under the pointer");
    glfwShowWindow(previews[1].window);
    settle();
    verify_state();

    check(glfwGetWindowAttrib(previews[1].window, GLFW_VISIBLE), 1,
          "Preview 2: is visible again");
    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
          "Preview 2: is hovered again because the pointer is over its content "
          "area");
    check(geometrical_pick(pointer_x, pointer_y) == 1, 1,
          "the pointer picks the re-shown preview");
    check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED) == 0, 1,
          "Preview 1: does not claim hover after the other preview is shown");

    //--------------------------------------------------------------------
    // The re-shown preview takes focus while the pointer is inside it; any
    // enter notification and the picking coordinates must belong to it
    //--------------------------------------------------------------------

    begin_step("the re-shown preview takes focus with the pointer inside it");
    check(acquire_focus(previews + 1), 1,
          "Preview 2: acquires input focus");
    settle();
    verify_state();

    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
          "Preview 2: remains the picking target after taking focus");
    check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED) == 0, 1,
          "Preview 1: is not the picking target because focus moved");
    check(previews[1].entered == 1, delivers(),
          "Preview 2: last enter/leave notification was entered=%s while the "
          "pointer is inside its focused content area",
          previews[1].entered ? "true" : "false");
    verify_untouched(previews + 0, "the pointer is not over its content area");

    //--------------------------------------------------------------------
    // The pointer leaves the re-shown preview into the gap and crosses back
    // in: the notifications must follow the pointer
    //--------------------------------------------------------------------

    begin_step("the pointer leaves the re-shown preview into the gap");
    move_pointer(gap_x, gap_y);
    verify_state();

    check(previews[1].step_leaves >= 1, delivers(),
          "Preview 2: leaving its content area produced %i leave "
          "notification(s)", previews[1].step_leaves);
    check(previews[1].entered == 0, delivers(),
          "Preview 2: last enter/leave notification was entered=%s while the "
          "pointer is between the previews",
          previews[1].entered ? "true" : "false");
    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED) == 0, 1,
          "Preview 2: is not hovered while the pointer is between the "
          "previews");
    verify_untouched(previews + 0, "the pointer is not over its content area");

    begin_step("the pointer crosses back into the re-shown focused preview");
    {
        double centre_x, centre_y;
        double before[PREVIEW_COUNT][2];
        double after[PREVIEW_COUNT][2];

        centre_of(previews + 1, &centre_x, &centre_y);
        move_pointer(centre_x, centre_y);
        verify_state();

        check(previews[1].step_enters >= 1, delivers(),
              "Preview 2: re-entering its content area produced %i enter "
              "notification(s)", previews[1].step_enters);
        check(previews[1].entered == 1, delivers(),
              "Preview 2: last enter/leave notification was entered=%s while "
              "the pointer is inside its content area",
              previews[1].entered ? "true" : "false");
        check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
              "Preview 2: is the picking target again");
        verify_untouched(previews + 0,
                         "the crossing did not involve its content area");

        // A displacement polled through the hovered and through the focused
        // preview must agree even when the other preview is not tracked
        poll_frames(before);
        move_pointer(centre_x + 12.0, centre_y + 6.0);
        verify_state();
        poll_frames(after);

        for (i = 0;  i < PREVIEW_COUNT;  i++)
        {
            check(close_enough(after[i][0] - before[i][0], 12.0) &&
                  close_enough(after[i][1] - before[i][1], 6.0), 1,
                  "%s: relative displacement polled through it was "
                  "(%0.1f, %0.1f) while the pointer moved by (12.0, 6.0)",
                  previews[i].title,
                  after[i][0] - before[i][0],
                  after[i][1] - before[i][1]);
        }
    }

    //--------------------------------------------------------------------
    // Showing a preview away from the pointer must not fabricate hover or an
    // enter notification for it
    //--------------------------------------------------------------------

    begin_step("the other preview is hidden and shown away from the pointer");
    glfwHideWindow(previews[0].window);
    settle();
    verify_state();

    check(glfwGetWindowAttrib(previews[0].window, GLFW_VISIBLE) == 0, 1,
          "Preview 1: is not visible after being hidden");

    glfwShowWindow(previews[0].window);
    settle();
    verify_state();

    check(glfwGetWindowAttrib(previews[0].window, GLFW_VISIBLE), 1,
          "Preview 1: is visible again");
    check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED) == 0, 1,
          "Preview 1: is not hovered after being shown away from the pointer");
    check(previews[0].step_enters == 0, 1,
          "Preview 1: received %i enter notification(s) when shown away from "
          "the pointer", previews[0].step_enters);
    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
          "Preview 2: remains the picking target while the other preview is "
          "shown elsewhere");

    //--------------------------------------------------------------------
    // Input focus moves to the preview that is not hovered while the pointer
    // stays over the other one; hover and picking must not follow the focus
    //--------------------------------------------------------------------

    begin_step("input focus moves to the preview that is not hovered");
    check(acquire_focus(previews + 0), 1,
          "Preview 1: acquires input focus");
    settle();
    verify_state();

    check(glfwGetWindowAttrib(previews[0].window, GLFW_FOCUSED), 1,
          "Preview 1: has input focus");
    check(glfwGetWindowAttrib(previews[1].window, GLFW_HOVERED), 1,
          "Preview 2: remains hovered and the picking target");
    check(glfwGetWindowAttrib(previews[0].window, GLFW_HOVERED) == 0, 1,
          "Preview 1: is not hovered merely because it has input focus");
    check(geometrical_pick(pointer_x, pointer_y) == 1, 1,
          "the pointer still picks the preview under it, not the focused one");
}

//========================================================================
// Main
//========================================================================

int main(int argc, char** argv)
{
    int i;
    int platform;
    double gap_x = 0.0, gap_y = 0.0;
    double start_x = 0.0, start_y = 0.0;
    Preview* origin_focus = NULL;

    for (i = 1;  i < argc;  i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage();
            return 0;
        }
        else if (strcmp(argv[i], "-w") == 0)
            wait_mode = 1;
        else if (strcmp(argv[i], "-p") == 0)
            pointer_led = 1;
        else
        {
            usage();
            return 1;
        }
    }

    glfwSetErrorCallback(error_callback);

    if (!glfwInit())
        return 1;

    platform = glfwGetPlatform();

    if (platform == GLFW_PLATFORM_WAYLAND)
    {
        printf("SKIP: Wayland does not expose window positions and does not "
               "support setting the cursor position\n");
        glfwTerminate();
        return EXIT_SKIP;
    }
    else if (platform == GLFW_PLATFORM_NULL)
    {
        printf("SKIP: the Null platform does not provide a real pointer\n");
        glfwTerminate();
        return EXIT_SKIP;
    }

    // Only the key window receives mouse tracking on Cocoa
    tracks_unfocused_windows =
        (platform == GLFW_PLATFORM_WIN32 || platform == GLFW_PLATFORM_X11);

    printf("Preview picking test\n");
    printf("Platform %s, tracking of unfocused previews %s\n",
           platform == GLFW_PLATFORM_COCOA ? "Cocoa" :
           platform == GLFW_PLATFORM_WIN32 ? "Win32" : "X11",
           tracks_unfocused_windows ? "expected" :
           "not promised; affected checks are warnings");

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    // Keep the previews above unrelated windows so that "the pointer is over
    // the content area with no other window between" is decided by the
    // previews themselves and not by an untidy desktop
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);

    if (!create_preview(previews + 0, "Preview 1", 0) ||
        !create_preview(previews + 1, "Preview 2", 1))
    {
        glfwTerminate();
        return 1;
    }

    if (!place_previews(&gap_x, &gap_y))
    {
        glfwTerminate();
        return EXIT_SKIP;
    }

    // Both previews are now visible but unfocused
    if (!acquire_focus(previews + 0))
    {
        printf("SKIP: could not give a preview window input focus; run this "
               "test on an interactive desktop session\n");
        glfwTerminate();
        return EXIT_SKIP;
    }

    if (!update_pointer())
    {
        printf("SKIP: could not determine the initial pointer position\n");
        glfwTerminate();
        return EXIT_SKIP;
    }

    probe_delivery(gap_x, gap_y);

    origin_focus = find_focused();
    start_x = pointer_x;
    start_y = pointer_y;

    if (pointer_led)
    {
        // The operator drives the pointer, so every crossing is real pointer
        // motion and notifications are delivered on every platform
        delivers_notifications = 1;
        run_pointer_led();
    }
    else
        run_warped(gap_x, gap_y);

    //--------------------------------------------------------------------
    // Restore the initial state without recording further checks
    //--------------------------------------------------------------------

    testing = 0;

    if (find_focused())
        move_pointer(start_x, start_y);

    if (origin_focus)
        acquire_focus(origin_focus);

    paint_all();

    printf("\n===========================================================\n");
    printf("%i checks, %i failures, %i warnings\n",
           check_count, failure_count, warning_count);
    printf("%s\n", failure_count ? "FAILED" : "PASSED");
    fflush(stdout);

    glfwTerminate();
    return failure_count ? 1 : 0;
}
