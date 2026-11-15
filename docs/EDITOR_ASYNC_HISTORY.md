# Editor tasks and command history

## Implementation analysis

The editor previously mixed worker computations with synchronous point-array edits and GPU-buffer
preparation. It also exposed a single busy flag to independent WSL and QtConcurrent operations.
The following failure cases determined the implementation:

| Risk | Resolution |
| --- | --- |
| QtConcurrent replaces an ordinary worker exception and loses its original diagnostic | Capture `std::exception_ptr` in the worker and transport it with the result. |
| Task startup, result retrieval, or the completion handler throws | The shared `RunBackground` boundary catches these paths. Scope-based cleanup releases the watcher and restores task state. |
| A multi-cloud callback adds some results before a later result fails | Completion callbacks stage model changes in a draft; publication occurs only after all point arrays and drawing buffers are ready. |
| Selection or layer edits race a pending result | The editor disables mutation controls, history controls, and new edit gestures while tasks or scene preparation are pending, and the viewer runs one edit at a time: an edit submitted meanwhile is queued, not dropped. Camera navigation remains available. |
| Undo copies every cloud or mutates a cached/shared source | History stores shared point arrays and immutable selection masks. Editing creates new arrays; metadata-only edits reuse the existing data. |
| Undo rebuilds millions of vertices on the UI thread | Restoring a history state uses the same worker preparation and GUI publication path as normal commands. |
| Reused row indices identify a different cloud after deletion/reordering | States preserve stable cloud IDs. New cloud IDs are never reused when history branches. |
| Undo arrives during a selection or gizmo drag | Cancel the uncommitted gesture before restoring history, so its later mouse release cannot overwrite the restored state. |
| Cancel is requested while an AI input file is being written | Check cancellation after preparation and before starting WSL. |
| A later cloud in an AI batch fails | Keep completed inference outputs staged until the whole batch succeeds. Failed/cancelled batches do not publish partial outputs. |
| Undo deletes or replaces exported files | Exports are read-only with respect to the scene and do not enter its history. Existing-file protection remains in the writers. |

## Worker and GUI responsibilities

`src/core/AsyncTask.h` is the common completion/error boundary. Worker functions capture data and
parameters, not widgets. Result payloads use shared ownership so large drawing buffers are not
copied when Qt delivers completion.

The following execute on workers: file loading, AI input preparation, algorithms, translations and
rotations, point deletion, rectangular point selection, recoloring buffers, history restoration,
point-cloud merging/export, CSV export, and training-data export.

The GUI thread owns dialogs, layer widgets, model publication, and OpenGL resources. Scene
publication reuses unchanged GPU entries and uploads changed vertex data with a total 32 MiB
budget per frame; a cloud whose new buffer is still uploading is drawn from the buffer it
replaces, and that one is released when the upload completes. Editing pivots and framing use cached counts, centroids, and bounds.
The navigation center is initialized once from the first cloud; manual pan remains separate.
Centroid alignment translates other selected visible clouds onto the first selected cloud
in panel order as one command, leaving its reference data unchanged.
Header merge prepares a new concatenated cloud on the worker and replaces selected visible
layers together, retaining their original data through history. Its output uses stored XYZ/RGB,
not preview color overrides. ICP result sets likewise publish as a single history command.
Radial selection commits a complete stroke on release through the same worker path. Its
temporary screen mask covers the swept brush area; existing selections retain their counts,
and unchanged strokes do not enter history. Escape/right-click or history navigation cancels
an unfinished stroke. Brush radius and camera navigation are UI settings outside command history.

## History contract

- Undo/Redo buttons and Ctrl+Z / Ctrl+Y (also Ctrl+Shift+Z).
- The history selector can restore any retained state.
- Up to 50 undoable commands, plus their oldest retained baseline state, within a memory
  budget of 2 GiB of point data and selection masks (see History limits below).
- A new edit after Undo discards the abandoned redo branch.
- Loading, cloud/point selection, deletion, transformations, visibility, colors, names, order,
  and processing results are recorded. A multi-cloud method application is one command.
- States restore point data, selection masks, selected cloud IDs, names, colors, visibility,
  details, and order. Camera orbit/pan/zoom and exported disk files are outside this history.
- Save Project persists the history and its current Undo/Redo position in a `.smcppce` file.
- Large geometric edits retain earlier point versions until those commands expire, the memory
  budget drops them, or the editor closes. Unchanged clouds/masks are shared.

## Editor projects

`PointcloudProject` stores a versioned UTF-8 JSON manifest followed by little-endian binary point
and selection assets and a SHA-256 checksum. Points use three float32 coordinates and three RGB
bytes, without native struct padding. Immutable cloud and mask pointers are deduplicated across
the current scene and every retained history state. Camera center, initialization flag, pan,
distance and orbit are saved separately from command history.
Rendering counts, centroids and bounds are rebuilt from the points, never trusted from a project file.

`QSaveFile` commits a complete project atomically. The reader bounds metadata to 16 MiB and checks
declared asset sizes against file length before allocating points. It validates checksums, versions,
layer IDs, selections, mask lengths and flags, history positions, colors and finite view metadata.
Only a fully decoded project is submitted for asynchronous buffer preparation; the viewer publishes
it together with its history. Read/preparation failures preserve the current session. Saving and
opening do not create extra history commands. A batch layer deletion is one normal scene command.

The project is self-contained and uncompressed. Shared snapshots occupy one point asset, while
geometric edits retain distinct point versions. Processing snapshots restore without rerunning WSL.

## Validation

`tests/PointcloudEditorTests.cpp` covers selection, hidden clouds, stable IDs, batch undo/redo,
exact point-data restoration, deleted-point selection restoration, the 50-command bound, redo
branch truncation, and injected worker/completion failures. Existing export protection tests remain.

A Windows GUI workflow also checked loading, multi-cloud outlier removal, translation, Undo/Redo,
and recovery after an injected error. A separate native run with 1,000,000 points kept a UI timer
active during preparation, transformation, selection, deletion, and history restoration. Observed
Debug times on the development machine were 338 ms for initial loading/preparation, 102 ms for
transformation and selection, 14 ms for deletion, and 39–47 ms for restoration. These are local
smoke measurements, not performance guarantees.

A native CSV/training export run with 200,000 labeled points checked the resulting measurements,
point count, manifest, UI responsiveness, existing-file protection, and control/cursor recovery
after success and failure.

Real WSL/GPU inference and long-running memory pressure across 50 large geometric edits still
need validation on the intended measurement datasets.

## Maintaining the editor

This section is the starting point for anyone changing the Point Cloud Editor.

### Who does what

| Piece | File | Responsibility |
|---|---|---|
| `PointcloudEditorDialog` | `src/ui/pointcloud_editor/PointcloudEditor*.cpp` | Controller of the window, one class in four files (see below) |
| `EditorScene` | `src/ui/preview/EditorScene.*` | The scene as plain data and the operations on it: selection, deletion, transforms, merging, history trimming. No widgets, no OpenGL |
| `PointcloudPreviewWidget` | `src/ui/preview/PointcloudPreview*.cpp` | Owns the current scene and its history, runs edits one at a time, draws and handles input. One class in three files (see below) |
| `PointcloudLayersPanel` | `src/ui/pointcloud_editor/PointcloudLayersPanel.*` | The tree of layers. It reads the viewer's layers and edits them through the viewer's public API; `SelectedLayerIds()` are the target clouds of the commands (highlighted and checked rows, or every checked row) |
| `PointcloudOps`, `SphereMeasurement`, `Compensation` | `src/core/*` | The numerical work. No Qt widgets; safe to call from a worker |
| `PointcloudProject` | `src/ui/pointcloud_editor/PointcloudProject.*` | Reading and writing `.smcppce` files |
| `RunBackground` | `src/core/AsyncTask.h` | Runs a function on a worker and delivers its result or exception on the GUI thread |

### Where the code is

`PointcloudEditorDialog` (declared in `PointcloudEditorDialog.h`):

| File | Contents |
|---|---|
| `PointcloudEditorDialog.cpp` | Construction and signal wiring, busy state, layer selection, tools |
| `PointcloudEditorCommands.cpp` | The PointCloud, Fit and Measure menu commands |
| `PointcloudEditorAi.cpp` | AI models in WSL: discovery, configuration, the inference batch |
| `PointcloudEditorFiles.cpp` | Loading clouds (and their cache), exporting, editor projects |

`PointcloudPreviewWidget` (declared in `PointcloudPreviewWidget.h`):

| File | Contents |
|---|---|
| `PointcloudPreviewWidget.cpp` | Scene transactions, layers, history, camera, OpenGL drawing |
| `PointcloudPreviewOverlays.cpp` | Transform gizmo, view selector, selection rectangle (QPainter) |
| `PointcloudPreviewInput.cpp` | Mouse, wheel and keyboard |
| `PointcloudPreviewInternal.h` | Constants and helpers shared by those three files |

What a scene operation *does* is in `EditorScene.cpp`; the widget only wraps it in an edit. To
change how a selection or a merge behaves, or to add a new operation on layers, start there: the
functions take a scene and return a scene, and `SceneOperationChecks` in the tests calls them
directly.

### The scene is a value

`PointcloudPreviewWidget::SceneState` is the list of layers (`CloudState`) plus the ids of the active
layers. A layer holds shared pointers to its point data and its selection mask, and **neither is
ever modified in place**: an operation that changes points builds a new array. That is what makes
undo cheap (snapshots share everything that did not change) and what lets a worker read a scene
while the GUI thread keeps drawing it.

A layer is identified by its **id** (`LayerInfo::id`), stable for its whole life. Its position in
the list changes when layers are added, removed or reordered. Keep ids, not positions, across
anything asynchronous: `FindLayer(id, …)`, `SetLayerColor(id, …)`, `IndexOfLayer(id)`.
`AddPointcloud` and `PrependPointcloud` return the id of the new layer.

### How one change travels

```text
Change() / EditScene()        a function "scene -> next scene" is submitted
  Submit()                    runs it now, or queues it behind the edit in progress
    StartEdit()               worker: the function, then PrepareScene() (GPU buffers of changed clouds)
                              GUI:    InstallScene(), then one history entry
    FinishEdit()              next queued edit, or idle -> BusyChanged(false)
```

- **One edit at a time.** An edit submitted while another is running waits in `_pendingEdits` and
  starts from the scene the previous one leaves. Nothing is dropped. If an edit fails, the edits
  queued behind it are discarded (they assumed it would succeed) and `OperationFailed` is emitted;
  scene and history stay as they were.
- **Commands group changes.** Between `BeginCommand(label)` and `EndCommand()` every layer call
  (`AddPointcloud`, `SetLayerColor`, …) changes a draft; `EndCommand()` submits the draft as one edit
  and therefore one history entry. `AbortCommand()` discards it. `EditScene()` is ignored while a
  command is open.
- **What is sent to the GPU.** Each layer has two vertex buffers: positions (12 bytes per point)
  and colors (4 bytes: red, green, blue and a "selected" flag).

  | Change | Rebuilt on the worker | Sent to the GPU |
  |---|---|---|
  | New point data (load, move, filter, delete points) | positions and colors | both |
  | Selection | colors | colors only; the positions buffer is shared with the replaced entry |
  | Layer color | nothing | nothing: it is a shader parameter (`u_override`) |
  | Rename, hide, reorder | nothing | nothing |

  Uploads are limited to 32 MiB per frame. While a replacement uploads, the entry it replaces
  keeps being drawn, so the cloud never disappears.
- **Drawing while the camera moves.** With more than 4 million visible points, orbit, pan and
  zoom draw about 2 million of them (one of every N, by widening the vertex stride, with no extra
  buffers) and the full scene returns 200 ms after the camera stops. The constants are in
  `PointcloudPreviewInternal.h`.

### Busy state

| Flag | Owner | True while |
|---|---|---|
| `PointcloudPreviewWidget::IsWorking()` | viewer | an edit is running or queued |
| `_viewerBusy` | dialog | mirror of the above, set from `BusyChanged` |
| `_busy` | dialog | a command of the dialog is running (worker or AI batch) |
| `IsWorking()` | dialog | `_busy` or `_viewerBusy`: actions, tools and the layer panel are disabled |

Everything that enables or disables controls is in `UpdateCommandAvailability()`; call `SetBusy()`
rather than touching `_busy`. The AI batch keeps the dialog busy across its steps through
`_aiInferenceRunning` and `_continueAi`; its sequence is described above the AI declarations in
`PointcloudEditorDialog.h`.

### Adding a command

1. **Core.** Put the computation in `src/core` as a function from input clouds and a parameter
   struct to new clouds. Do not modify the input: return a new `ColorCloudPtr`. Add a test in
   `tests/PointcloudOpsTests.cpp`.
2. **Parameters.** If the user must choose values, add a small dialog next to the others in
   `src/ui/pointcloud_editor` (a `.ui` form plus a class with `Params()`). Name its buttons
   `done_button` and `cancel_button` so `EditorDialogStyle` sizes them and marks the primary one.
3. **Menu.** Add a `QAction` to `PointcloudEditorDialog.ui` and a slot
   `on_<name>_action_triggered` in the dialog. Add the action to the list that
   `UpdateCommandAvailability()` enables and disables.
4. **Slot.** Follow this shape (compare `on_remove_outliers_action_triggered`):

   ```cpp
   void PointcloudEditorDialog::on_my_command_action_triggered(bool)
   {
       if (IsWorking())
           return;
       MyDialog dialog(_myParams, this);            // parameters
       if (dialog.exec() != QDialog::Accepted)
           return;
       _myParams = dialog.Params();
       const auto params = _myParams;               // copies for the worker; never capture `this` there

       RunSelected<Pointcloud::ColorCloudPtr>(
           tr("Running my command..."),             // status text and history label
           [params](const CloudLayer& layer)        // worker thread: no widgets
           { return Pointcloud::MyCommand(*layer.cloud, params); },
           [this](const CloudLayer& layer, const Pointcloud::ColorCloudPtr& result)   // GUI thread
           {
               preview_widget->PrependPointcloud(result, QColor(), tr("(Mine) %1").arg(layer.name),
                   tr("My command applied to %1").arg(layer.name));   // shown in the layer details
           });
   }
   ```

   `RunSelected` runs the work once per selected visible cloud and publishes all results as a
   single undoable command. Use `RunAsync` when the command is not "one result per cloud" (ICP,
   sphere measurement). An exception thrown in either lambda reaches `FailTask()`, which shows
   the message and leaves the scene untouched.

   **Make a long command cancellable.** Give the core function a trailing
   `const Pointcloud::CancelFlag* cancel = nullptr` parameter and call
   `Pointcloud::ThrowIfCanceled(cancel)` once per iteration of its outer loop (never from inside a
   helper thread: stop the thread's loop and throw after joining, as `SearchKnn` does). In the
   slot, call `NewCancelFlag()` right before `RunSelected`/`RunAsync`, capture the returned
   pointer in the worker lambda and pass `cancel.get()` to the core. The **Cancel command**
   button then appears while the command runs; pressing it makes the core throw
   `OperationCanceled`, which `FailTask()` reports as "Command canceled." without an error box.
   Nothing is published, so there is nothing to undo.
5. **Test.** Add a function to `tests/PointcloudEditorTests.cpp` and list it in `groups[]`.

### History limits

Undo keeps at most `Max_History_Entries` (50) commands **and** at most
`SetHistoryMemoryBudget()` bytes (2 GiB by default) of distinct point data and selection masks
across all its entries. When either limit is exceeded the oldest entries are dropped; the current
state and one step to undo are always kept. Dropping history never changes the scene.

### Tests

`tests/PointcloudEditorTests.cpp` runs one function per feature (`groups[]` in `main`). Each
builds its own viewer and data, so a failing group does not disturb the others, and its name is
printed. They run on Qt's offscreen platform, which has no OpenGL: drawing is checked separately
by `SMCP_viewer_render_check`, and the visual style by `SMCP_style_gallery`. The render check
draws a 5.3 million point cloud frame by frame and verifies, from the pixels, each row of the
table above, the no-blink replacement, undo, and the decimation while navigating; it also prints
the frame time at rest and while navigating. Run it after any change to the drawing code.

### Known debts

- `PointcloudPreviewWidget` and `PointcloudEditorDialog` are each still one class, now spread over
  several files by responsibility. The scene operations are already independent
  (`EditorScene`); the mouse tools (rectangle, brush, gizmo) are the next candidates to become
  classes of their own, and the commands to become objects instead of slots.
- Outlier removal, plane and sphere fitting, ICP and sphere measurement can be cancelled.
  Compensation, loading, exporting and project files cannot.
- The AI batch is a sequence of callbacks driven by several flags; it would be clearer as one
  object with an explicit state.
- Points are drawn with a fixed size of 2 pixels and no level of detail beyond the decimation
  used while navigating.
