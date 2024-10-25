# zui
A simple, tiny, extendable and straightfoward ui library in C

The library is available as a single-header library in [zui-sh.h](zui-sh.h)

Single header backends potentially coming soon

## Why zui?

`zui` is a super lightweight immediate-mode UI library purpose built for creating applications. Many other immediate-mode UI libraries exist: `ImGui`, `nuklear`, `microui`, etc, but all of them are more focused on creating debug UI's overtop of existing render passes, rather than being used for full UI applications. This is easily seen by the notion of `window` which doesn't represent a window, but rather a movable popup container. Creating a `window` which actually represents your window is rather awkward. `zui` will eventually have many features for debug UIs as well.

Immediate-mode ui libraries tend to calculate the position and size of UI elements as they're created. This is efficient and simple, but it severely undercuts the ability to have smart layouting systems. As a result, many times I've had to calculate positions or sizes of elements manually before creating them, in a cumbersome and awkward developer experience.

In `zui`, the sizes and positions of elements are calculated after the UI tree has been completed. This results in a lot of the heavily lifting of layouts being done by the widgets themselves. This means text justification, sizing for percentage of available space, and auto-sizing elements to contents is *super* simple.

## Architecture

All the memory `zui` uses is stored directly in the `zui_ctx` struct or within its pointers. Rather than the ui tree being a proper tree, the `ui` buffer is simply a contiguous buffer of all the widget data. Each ui element stores 3 things, an `id` setting the widget type, a `bytes` field marking its size, and a `next` field, which is an index into the `ui` buffer.

The reason that the ui buffer can be treated as a tree is because of how the `next` field is used. `next` represents a `ui` elements' sibling in the tree. Since all elements are stored in the order they were created, a child element will be directly after its parent, and we can used the `next` flag to quickly iterate through the rest of the children. The `next` of the last child will be an index back to the parent. This memory structure allows some very efficient use cases. Instead of storing many pointers for parent, child, sibling relationships, we only need one 4-byte index. Along with that, detecting whether a widget is a descendant of another widget is a *very* cheap operation due to everything being in contiguous memory.

Memory allocations are minimal as well. Since the `ui` buffer stores all widgets and is reused every frame, it only needs reallocation if the buffer isn't large enough. Since the `next` field is an index into the buffer, even if a reallocation changes the address of the `ui` buffer, it doesn't invalidate any widget relationships. Since memory usage is so small, there will be a future option which allows `zui` to only use static memory for widgets, as well as add custom memory allocation strategies for the buffers.

The core of the library is extremely minimal, mostly consisting of helper functions for widget behavior. Almost all of the ui logic is done within the widgets themselves, creating a very easy learning curve and developer experience to create your own widgets. The `zui_ctx` struct contains another buffer `registry` which contains a set of `zui_type` structs which describe widget behavior. It contains 3 function pointers:

1. size() - receives bounds, and returns the size the widget used
2. pos() - used by containers to calculate the positions of children
3. draw() - 'draws' the widget by adding commands to the `draw` buffer

Each widget `id` is associated with a `zui_type` defining its behavior. `zui` registers its own built-in elements in `zui_init()`. You can register your own elements and use them seamlessly in the library by adding the widget data to the `ui` buffer and registering its `id` via `zui_register()`.

Many immediate-mode ui libraries use hashing mechanisms to store state in the background, `zui` takes a different approach. `zui` does not store any widget state at all. Instead, when a user creates a widget, if the widget requires state, the user must pass a reference to a state variable. While in some cases this is not as clean as a hashing mechanism, I'd argue it's an excellent tradeoff. In 90% of cases, the user can declare a static state variable right before the widget creation. In the case of an array of elements with similar names, instead of having an awkward system to generate unique ids for each element, you can just index into an array of state variables. And you also don't have accidental state collisions by having two text boxes with identical tooltip text.

Basically: User allocates state. `zui` manages state.

## Planned Features

Backends
- [x] Sokol

Layouts:
- [x] Box
- [x] Row / Column
- [ ] Grid

Widgets:
- [x] Label
- [x] Text Input (selection is WIP)
- [x] Combo Box
- [x] Button
- [ ] Slider
- [ ] Text Box
- [ ] Popups
- [ ] Checkbox

Other
- [ ] Theme Support
- [ ] Common Built-in Icons
- [ ] Better DPI Font backends
- [ ] Rending Effects / Better animation support
