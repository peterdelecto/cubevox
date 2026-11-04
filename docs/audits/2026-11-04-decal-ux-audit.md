# Image Decal and Text Decal, user experience audit (2026-11-04)

Owner's ask, the same day. Write the two add-ins' spec from the user's point of view, step by
step, anticipating intent and goal at each step, then audit it for experience and intent. The
spec is `docs/specs/2026-11-04-image-text-decal-design.md`. Every finding below that changed
the spec is marked there with its number. Nothing here is built.

## How the audit ran

1. The lead walked nine scenarios against the spec as the owner would meet them, one dialog
   step at a time, asking at each step whether the user knows what to do next, whether the
   add-in did what was meant, where the user guesses or detours, and what the user wanted that
   was not offered. Those are the A findings.
2. A second reader with no part in writing the spec walked the same scenarios cold and wrote
   `build/audit/decal-ux-coldread.md`. Its findings are the B findings, merged below with what
   was done about each.

## Scenarios

1. The cane weave PNG over the whole lid top, keep-outs on, black.
2. A 300 px logo PNG, 30 mm wide, 10 mm in from the lid's corner, a hole close by.
3. "cubevox" in a chosen font, bold, 8 mm capitals, centred on the front wall, white.
4. "VOLUME" curved round a knob hole, upright at the bottom of the hole.
5. The cane edited after a hole moved, with the name decal already on the lid; then the name
   deleted.
6. Nothing selected, Image Decal, a wrong file, then a PNG, then the missing face.
7. A word wrapped round the Ø40 mm boss and moved to its top end.
8. Esc and Cancel at every stage, including mid-bake and after Preview the cut.
9. Undo right after OK.

## A findings, the lead's walk

| № | Scenario, step | What the user hit | Done in the spec |
|---|---|---|---|
| A1 | 2, step 2 | A 300 px logo opened blown up to the face width, blurred, with an upscaling warning before anything was asked | Default width is the fitted width or 0.3 mm per source pixel, whichever is smaller, and the readout says which |
| A2 | 2, step 3 | "10 mm from the corner" meant arithmetic from the face centre | Readout line 1 gives the distance to the two nearest face edges |
| A3 | 1, dialog | The Place group ran to eight rows with Width and Height both shown and the triad's Flip row uncounted | Height shows only while Keep proportions is off; the triad's rows are counted as Fusion shows them; twelve rows at open holds |
| A4 | 6, steps 1–2 | Choosing the image before the face showed nothing and said nothing | The image loads either way, the thumbnail shows, the readout says "Click a face to see it there", and the decal appears when the face is picked |
| A5 | 5, step 10 | To edit a decal the user had to click the lid beside it and find it in a dropdown, when the natural move is to click the decal | A click on a decal's inlay body opens that decal for editing |
| A6 | 5 | After a hole moved, the stale decal looked fine on screen and would have printed wrong | Each record carries a face fingerprint; at open the readout names stale decals and the change found |
| A7 | 5, and 1 + 3 together | The earlier spec suppressed every earlier decal on a body when a new one was cut, so the lid could not carry the cane and a name at once | Decals stack on a body; an edit re-cuts the stack above it; the readout names the count and time; the old rule is withdrawn in both specs |
| A8 | 1 + 3 | Two overlapping decals left the overlap in the earlier colour with no word | Orange warning with the earlier decal's name and the overlap area |
| A9 | 3, step 5 | Lines and Anchor were two controls for one intent, and the user would have to work out which to touch | One Align control sets both the line alignment and the point the triad holds |
| A10 | 4, step 6 | Centring curved text on a knob hole meant eyeballing the triad onto the hole's centre | The arc group gains Around, a pick of a round hole; the arc centre snaps to it, the radius follows the hole, and the ring turns the words round it |
| A11 | 7, step 6 | Along an arc had no meaning on a cylinder, which wraps already | The group is shown on flat faces only |
| A12 | 3, dialog | The triad's Flip row pushed Text Decal to thirteen rows for a control almost never used on text | Flip hidden for text; Mirror sits in Options |
| A13 | 1, step 3 | A first-time user may not know the dot, ring and handle can be dragged | The readout's first line says so until the first drag |
| A14 | 1, step 7 | Preview the cut sat in a collapsed group and the user facing a 50 s bake would not find it | When the bake is over 10 s the readout points to it |
| A15 | all | The lid is a component inside the enclosure, and the first build placed on the root | Placement arithmetic carries the occurrence transform; sketch and bake in the component |
| A16 | 3, step 4 | "cubevox" has no capitals, so 8 mm capital height makes 5.6 mm letters and the user wonders why | Readout line 1 shows the box the words make beside the height typed |

## B findings, the cold read

The second reader's report is `build/audit/decal-ux-coldread.md`, 40 findings (3 block the
task, 27 force a detour, 10 polish) and 25 contradictions. Every one changed the spec; none
was set aside. The three that blocked were B22 (curved words under a hole could not read
upright), B24 (the stale check read the top decal's frozen copy, where no hole ever moves) and
B25 (a name over the cane kept black strands through the white letters).

| № | Severity | What the user hit | Done in the spec |
|---|---|---|---|
| B1 | detour | Counting every row Fusion draws, both dialogs opened over 600 px | Rule 1 counts every row; 420 px wide; the triad's Scale and Flip rows are gone; both tables end with the count and the pixels |
| B2 | detour | Opening the groups ran the dialog off the screen again | One group open at a time; the tallest state is named under each table |
| B3 | detour | Messages of 80–90 characters wrapped a 360 px readout | 420 px; every message at most 70 characters; the readout is full width; line 4 shows the worst and "+n more" with the rest in its tooltip |
| B4 | polish | Options and Cut did not say what was inside | Groups renamed by their rows; Keep proportions stays in Depth and print limits because it is rare and a row at open costs more; Preview the cut stays in its group and line 3 points to it |
| B5 | detour | "Keep the cut as a stored body" hid that the decal stops following the face | Freeze the cut, with the tooltip saying the cost; on by default only when the bake is over 10 s |
| B6 | polish | Threshold, Shading, Minimum area, points, Tracing… | Ink, Photo relief, Smallest speck, Smallest line, Updating…; points only near a ceiling |
| B7 | polish | The decal was named after the file, "proof", and messages said "the cane decal" | A Name row, default the file stem or the words |
| B8 | detour | A big PNG decoded on Fusion's thread with nothing said | Decode in the worker, "Reading…" first, a 25 million pixel cap, a budget row |
| B9 | detour | Over a ceiling the red line did not say which row brings it down | The line names the row and the value, from one extra plan run |
| B10 | detour | "Centre on click" read as "click where it goes" and did something else | Place by click arms one click on the face |
| B11 | detour | The triad's rows might read the last drag, and Scale duplicated Width | The triad is made once at the face centre so its rows read position; scaling hidden; Width has its own arrow; cylinders get Round and Along rows |
| B12 | detour | "10 mm from the corner" and "5 mm from the hole" stayed arithmetic | Measure from (centre or a corner); the nearest hole's clearance on line 2 |
| B13 | detour | A white logo on transparent gave a one-tone message that pointed at the slider | The message names Light areas when the art is lighter |
| B14 | detour | Each keystroke queued a half-second layout | Layout after a 0.3 s pause; rule 2 restated; budget row |
| B15 | detour | Hundreds of fonts, no type-ahead, half a second each | Find a font… shows a filter row on demand; ◀ ▶ step through fonts |
| B16 | detour | Return might press OK mid-word | Shift+Return for a new line, proven in build step 1, said in the tooltip |
| B17 | polish | White pieces at 90 % on a light grey body did not show | Each piece edged in the opposite tone |
| B18 | polish | An empty text box put a red line up at open | A plain hint; OK disabled without red; rule 6 amended |
| B19 | polish | No first-run font or folder; B on a font with no bold face | Arial or the first font; the home folder; a bold face is detected by comparing loops and its absence said in orange |
| B20 | detour | Picking the hole set a 4.7 mm radius under a 17 mm height, red at once | Around sets radius and reduces height together so the words sweep at most 120° |
| B21 | detour | The knob covered the hole's edge and Around took only edges on the face | Around takes any circular edge; its centre is projected onto the face |
| B22 | blocks | No setting gave upright words under the hole; Inside sat inside the hole | Words read Over the hole or Under the hole, both outside the hole, both upright; the triad sits at the hole centre with the ring only; Angle shifts ±90° |
| B23 | detour | A frozen lid did not change on screen when a hole moved, and nothing said why | The status line says so at OK; the freeze default by estimate keeps small decals live; a global hook is in Not in this build |
| B24 | blocks | The stale check and the edit preview read the top frozen copy | Both read the body just before the lowest decal by a timeline marker round trip, budget 1 s, measured in build step 1 |
| B25 | blocks | Black cane strands ran through white letters while the preview drew the name whole | Clear earlier decals under it, on by default, re-cuts the earlier decals with the new outline as a keep-out, and the preview hides what it will clear |
| B26 | detour | A click on a strand of the full-face cane jumped into editing it | A click on an inlay body picks the face under it, keeps New, lists that decal first and says so |
| B27 | detour | Image Decal could not list or delete the name | Both Edit rows list every decal; Delete works on either kind |
| B28 | detour | Delete's timing, confirmation and undo were unsaid | Delete hides the decal in the preview, line 4 says what OK will do, it happens on OK as one undo step; it sits in the Place button row to keep the row count |
| B29 | detour | A JPEG was greyed in the file dialog with no reason | JPEG passes the filter and is refused by name with the fix |
| B30 | polish | Both buttons sat at the bottom of the Modify dropdown | Both are promoted onto the panel face |
| B31 | detour | A drag round the boss moved less than the mouse and could not pass a quarter turn | A drag along x is arc length |
| B32 | detour | A new face pick might reset everything | Every value but the place survives; the decal is centred again; the same in Edit |
| B33 | polish | The fit hung off a round face; a boss with its axis down the screen opened upside down | The fit runs on the face's mask; the upright rule covers cylinders |
| B34 | detour | Preview the cut had no Cancel, and a nudge afterwards lost the 50 s silently | Its progress bar has Cancel; line 4 says the cut preview was dropped and what OK costs |
| B35 | detour | A trace result arriving after Cancel could draw on the model | The command's end bumps the generation; the handler drops results when no command is live |
| B36 | detour | Esc while typing lost the words, font and place | The settings file is written on Cancel too; the text box opens with the last words selected |
| B37 | polish | "Cancel adds nothing" was wrong for an edit | Cancel or Esc during any bake leaves every decal as before OK; rule 5 restated |
| B38 | detour | A failed new file or font went red while a good decal stood on the face | Orange while the earlier file or font is in use |
| B39 | polish | Several lines named no fix | Every message has its fix with a number; the interlace fix names Preview's export |
| B40 | detour | A missing file higher in the stack failed mid-run | Edit checks every file and font in the run and names any missing in red before OK |

## Contradictions the cold read found, and where each is settled

| № | Was | Now |
|---|---|---|
| 1 | Image table said three readout lines, elsewhere four | Four everywhere, in section The readout |
| 2 | Four claims on the readout's first line | Line 1 face, line 2 place, line 3 count, line 4 message; hints live on line 4 |
| 3 | Clearing the face greyed the dialog, yet an image could come first | Clearing the face greys only Place |
| 4 | Rule 8 banned "threshold" and a row was named Threshold | The row is Ink |
| 5 | Rule 2's quarter second against 0.5 s budgets | Rule 2 asks for something to move in a quarter second and the clean result within the budget |
| 6 | Rule 4's "as large as fits" against a fifth for words and the pixel cap | Rule 4 says a sensible size and names both |
| 7 | Rule 1 allowed Edit alone beyond twelve, tables added Delete | Delete sits in the button row; rule 1 recounted |
| 8 | The Image count said twelve and listed eleven | Recounted, fifteen with the readout, with the pixels |
| 9 | Scale 1.00 at open against a logo opening below the fit | The Scale row is gone |
| 10 | Rule 3 had flip by handle, text had a Mirror tick | Rule 3 names mirror as a tick |
| 11 | Rule 5's one undo against an edit's run | One undo takes back everything OK did |
| 12 | The glance table listed anchor and line alignment apart | One Align control |
| 13 | Text step numbers collided with Image's | Text's own steps are T2 to T6; the shared steps are named |
| 14 | Text's Place said "as Image" without Width or Fit | Said outright in T5 |
| 15 | The Inside radius sat inside the hole | Over the hole and Under the hole, both outside it |
| 16 | The stale check read the face as it is now | The marker round trip reads the body before the lowest decal |
| 17 | The preview drew the later decal whole while the cut kept the earlier colour | Clear earlier decals under it, and the preview matches either setting |
| 18 | Cancel during an edit's bake "adds nothing" | Cancel leaves every decal as before OK |
| 19 | The earlier spec's message boxes | Messages live in the readout; one box for a bake failure; the earlier spec's header says so |
| 20 | The earlier table refused more than one face | The first is taken with an orange note; marked in the Errors table |
| 21 | The earlier table refused too-wide art and one-tone images | Clipped or warned in orange; marked in the Errors table |
| 22 | The earlier pipeline centred on the click and allowed quarter turns | Changes to the earlier pipeline 1 and 3 |
| 23 | The earlier cylinder rule fixed the tangent point at the click | Changes to the earlier pipeline 3 |
| 24 | The earlier bake drew the preview orange | Changes to the earlier pipeline 10 |
| 25 | A failed new file was red with the old one in use | Orange (B38) |

## Checked and found no change needed

1. Undo after OK (scenario 9) removes the group and, when a stack was re-cut, the whole run,
   because the command is one undo step.
2. The cylinder walk (scenario 7), once B31 and B33 were folded in. Dragging along the axis
   moves the word to the top end; the triad re-seats on the surface after each drag; width is
   arc length.

## Open for the owner

1. Filament colours.
2. The default arc radius when nothing is picked in Around, once the first knob label is
   placed.
