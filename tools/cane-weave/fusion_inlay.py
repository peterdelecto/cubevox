"""A lid inlay pattern built in Fusion through the MCP script runner.

Reads build/cane/<name>-inlay.json (from inlay_export.py) and adds to the Lid
component one sketch `<name>` on the top face, one extrude `<name> cut` 0.4 mm into
the lid, one base feature (`<name> dots` for circles, `<name> inlay` for polygons)
holding the pieces as a single body, and a timeline group `<name>`. Each stage is skipped when its feature already exists, so a
rerun after an MCP timeout is safe.

Circles go the cylinder route: a union of temporary cylinders added to the base
feature. Polygons are drawn as closed line loops; the light profiles are the ones
whose area and centroid match a piece in the JSON. Their inlay body fills the
pockets the cut left: a temporary slab over the field less the cut lid, the
through-holes and the window, added to the base feature.

The MCP runner calls run(); NAME and STOP_AFTER pick the pattern and the last stage.
Another script may import this file and call build(), verify(), snapshot() or
suppress() directly.
"""

import json
import math

import adsk.core
import adsk.fusion

JSON_DIR = '/Users/j/Documents/Claude/Projects/cubevox/build/cane'
DOC_NAME = 'cubevox-enclosure'
DEPTH_MM = 0.4           # must equal the user parameter inlayDepth
LID_MM3 = 76234.35       # Lid body volume with no inlay group, read 2026-11-04
NAME = 'C12'
STOP_AFTER = None        # 'sketch' | 'cut' | 'dots' | None (run everything)

# Height of the lid's top face, mm: from the largest flat face looking up while
# the sketch does not exist yet, from the sketch plane after.

FACE_Z_MM = None


def design_and_lid():
  app = adsk.core.Application.get()
  if app.activeDocument.name.split(' v')[0] != DOC_NAME:
    raise RuntimeError('active document is %s' % app.activeDocument.name)
  design = adsk.fusion.Design.cast(app.activeProduct)
  for occ in design.rootComponent.allOccurrences:
    if occ.component.name == 'Lid':
      return design, occ, occ.component
  raise RuntimeError('no Lid component')


def load(name):
  with open('%s/%s-inlay.json' % (JSON_DIR, name.lower())) as fh:
    return json.load(fh)


def top_face(body):
  """The largest flat face of the body that looks up; sets FACE_Z_MM from it."""
  global FACE_Z_MM
  best = None
  for f in body.faces:
    g = f.geometry
    if g.objectType == adsk.core.Plane.classType() and abs(g.normal.z - 1) < 1e-6:
      if best is None or f.area > best.area:
        best = f
  if best is None:
    raise RuntimeError('no flat face looking up')
  FACE_Z_MM = round(best.pointOnFace.z * 10, 4)
  print('top face z mm', FACE_Z_MM, 'area mm2 %.1f' % (best.area * 100))
  return best


def feature_named(collection, name):
  for i in range(collection.count):
    if collection.item(i).name == name:
      return collection.item(i)
  return None


def group_named(design, name):
  for g in design.timeline.timelineGroups:
    if g.name == name:
      return g
  return None


# --- Pattern geometry ---------------------------------------------------------

def ring_area_centroid(pts):
  """Signed area (mm2) and area moments of a closed loop of [x, y] points."""
  a = cx = cy = 0.0
  n = len(pts)
  for k in range(n):
    x0, y0 = pts[k]
    x1, y1 = pts[(k + 1) % n]
    c = x0 * y1 - x1 * y0
    a += c
    cx += (x0 + x1) * c
    cy += (y0 + y1) * c
  return a / 2, cx / 6, cy / 6


def pieces(data):
  """Area (mm2) and centroid (lid mm) of each polygon, holes subtracted."""
  out = []
  for poly in data['polygons']:
    a, mx, my = ring_area_centroid(poly['outer'])
    for h in poly['holes']:
      ha, hx, hy = ring_area_centroid(h)
      a, mx, my = a + ha, mx + hx, my + hy
    out.append((a, mx / a, my / a))
  return out


def expected_mm3(data):
  if data['kind'] == 'circles':
    return sum(math.pi * r * r * DEPTH_MM for _, _, r in data['circles'])
  return data['area'] * DEPTH_MM


def count(data):
  return len(data['circles']) if data['kind'] == 'circles' else len(data['polygons'])


def body_name(name, data):
  """The inlay body and its base feature: `<name> dots` for circles, `<name> inlay` for polygons."""
  return name + (' dots' if data['kind'] == 'circles' else ' inlay')


# --- Sketch -------------------------------------------------------------------

def to_sketch(sk, x, y):
  p = sk.modelToSketchSpace(adsk.core.Point3D.create(x / 10, y / 10, FACE_Z_MM / 10))
  p.z = 0
  return p


def draw_loop(sk, pts):
  lines = sk.sketchCurves.sketchLines
  first = prev = to_sketch(sk, *pts[0])
  start_point = None
  for x, y in pts[1:]:
    ln = lines.addByTwoPoints(prev, to_sketch(sk, x, y))
    if start_point is None:
      start_point = ln.startSketchPoint
    prev = ln.endSketchPoint
  lines.addByTwoPoints(prev, start_point if start_point else first)


def sketch_z(sk):
  """Sets FACE_Z_MM from the sketch plane. Once the cut has holed the top face,
  a lower face is larger, so top_face() no longer finds it."""
  global FACE_Z_MM
  FACE_Z_MM = round(sk.sketchToModelSpace(adsk.core.Point3D.create(0, 0, 0)).z * 10, 4)
  print('sketch plane z mm', FACE_Z_MM)


def stage_sketch(comp, lid, name, data):
  sk = feature_named(comp.sketches, name)
  if sk:
    print('sketch %s exists, curves %d' % (name, sk.sketchCurves.count))
    sketch_z(sk)
    return sk
  sk = comp.sketches.add(top_face(lid))
  sk.name = name
  sk.isComputeDeferred = True
  if data['kind'] == 'circles':
    add = sk.sketchCurves.sketchCircles.addByCenterRadius
    for x, y, r in data['circles']:
      add(to_sketch(sk, x, y), r / 10)
  else:
    for poly in data['polygons']:
      draw_loop(sk, poly['outer'])
      for h in poly['holes']:
        draw_loop(sk, h)
  sk.isComputeDeferred = False
  print('sketch %s drawn, curves %d, profiles %d' % (name, sk.sketchCurves.count, sk.profiles.count))
  return sk


# --- Profiles -----------------------------------------------------------------

def circle_profiles(sketch):
  out = adsk.core.ObjectCollection.create()
  for p in sketch.profiles:
    if p.profileLoops.count != 1:
      continue
    loop = p.profileLoops.item(0)
    if loop.profileCurves.count != 1:
      continue
    ent = loop.profileCurves.item(0).sketchEntity
    if ent.objectType == adsk.fusion.SketchCircle.classType() and ent.radius <= 0.2:
      out.add(p)
  return out


def polygon_profiles(sketch, data):
  """Profiles whose area and centroid match a piece: the light ones. A hole's
  island and any projected face outline match nothing."""
  cell = 0.05
  table = {}
  for k, (a, x, y) in enumerate(pieces(data)):
    table.setdefault((round(x / cell), round(y / cell)), []).append((k, a, x, y))
  out = adsk.core.ObjectCollection.create()
  used = set()
  accuracy = adsk.fusion.CalculationAccuracy.HighCalculationAccuracy
  for p in sketch.profiles:
    ap = p.areaProperties(accuracy)
    c = sketch.sketchToModelSpace(ap.centroid)
    x, y, a = c.x * 10, c.y * 10, ap.area * 100
    ki, kj = round(x / cell), round(y / cell)
    hit = None
    for di in (-1, 0, 1):
      for dj in (-1, 0, 1):
        for k, pa, px, py in table.get((ki + di, kj + dj), ()):
          if math.hypot(px - x, py - y) < 0.01 and abs(pa - a) < 0.005 * pa:
            hit = k
    if hit is not None and hit not in used:
      used.add(hit)
      out.add(p)
  return out


def light_profiles(sketch, data):
  profs = circle_profiles(sketch) if data['kind'] == 'circles' else polygon_profiles(sketch, data)
  if profs.count != count(data):
    raise RuntimeError('light profiles %d, expected %d' % (profs.count, count(data)))
  return profs


# --- Cut ----------------------------------------------------------------------

def stage_cut(comp, lid, sk, name, data):
  ext = comp.features.extrudeFeatures
  f = feature_named(ext, name + ' cut')
  if f:
    print('extrude %s cut exists' % name)
    return f
  profs = light_profiles(sk, data)
  before = lid.volume
  inp = ext.createInput(profs, adsk.fusion.FeatureOperations.CutFeatureOperation)
  inp.setDistanceExtent(False, adsk.core.ValueInput.createByString('-inlayDepth'))
  inp.participantBodies = [lid]
  f = ext.add(inp)
  f.name = name + ' cut'
  print('extrude %s cut, lid volume mm3 %.1f -> %.1f' % (name, before * 1000, lid.volume * 1000))
  return f


# --- Dots ---------------------------------------------------------------------

def union_all(parts):
  """Pairwise union of temporary bodies into the first."""
  tmp = adsk.fusion.TemporaryBRepManager.get()
  while len(parts) > 1:
    nxt = []
    for i in range(0, len(parts) - 1, 2):
      if not tmp.booleanOperation(parts[i], parts[i + 1], adsk.fusion.BooleanTypes.UnionBooleanType):
        raise RuntimeError('union failed')
      nxt.append(parts[i])
    if len(parts) % 2:
      nxt.append(parts[-1])
    parts = nxt
  return parts[0]


def cylinders(data):
  tmp = adsk.fusion.TemporaryBRepManager.get()
  zt, zb = FACE_Z_MM / 10, (FACE_Z_MM - DEPTH_MM) / 10
  return [tmp.createCylinderOrCone(adsk.core.Point3D.create(x / 10, y / 10, zb), r / 10,
                                   adsk.core.Point3D.create(x / 10, y / 10, zt), r / 10)
          for x, y, r in data['circles']]


def box(x0, y0, x1, y1, z0, z1):
  """Temporary box between two corners, mm."""
  obb = adsk.core.OrientedBoundingBox3D.create(
    adsk.core.Point3D.create((x0 + x1) / 20, (y0 + y1) / 20, (z0 + z1) / 20),
    adsk.core.Vector3D.create(1, 0, 0), adsk.core.Vector3D.create(0, 1, 0),
    (x1 - x0) / 10, (y1 - y0) / 10, (z1 - z0) / 10)
  return adsk.fusion.TemporaryBRepManager.get().createBox(obb)


def pockets(lid, data):
  """The pockets the cut left in the face as one temporary body: a slab over the
  field from the face to 0.05 mm under the cut floor, less the cut lid, the
  through-holes and the window. Voids come out 0.05 mm wide of their walls;
  the pattern stays 0.2 mm off them, so no piece is clipped."""
  tmp = adsk.fusion.TemporaryBRepManager.get()
  top, low = FACE_Z_MM, FACE_Z_MM - DEPTH_MM - 0.05
  slab = box(*data['field'], low, top)
  tools = [tmp.copy(lid)]
  for x, y, dia in data['face_holes']:
    r = (dia / 2 + 0.05) / 10
    tools.append(tmp.createCylinderOrCone(adsk.core.Point3D.create(x / 10, y / 10, (low - 1) / 10), r,
                                          adsk.core.Point3D.create(x / 10, y / 10, (top + 1) / 10), r))
  x0, y0, x1, y1 = data['window']
  tools.append(box(x0 - 0.05, y0 - 0.05, x1 + 0.05, y1 + 0.05, low - 1, top + 1))
  for t in tools:
    if not tmp.booleanOperation(slab, t, adsk.fusion.BooleanTypes.DifferenceBooleanType):
      raise RuntimeError('pocket subtraction failed')
  want, got = expected_mm3(data), slab.volume * 1000
  if slab.lumps.count != count(data) or abs(got - want) > 0.001 * want:
    raise RuntimeError('pockets: lumps %d of %d, volume mm3 %.2f of %.2f'
                       % (slab.lumps.count, count(data), got, want))
  return slab


def stage_dots(comp, lid, name, data):
  piece = body_name(name, data)
  bf = feature_named(comp.features.baseFeatures, piece)
  if bf:
    print('base feature %s exists, lumps %d' % (piece, bf.bodies.item(0).lumps.count))
    return bf
  acc = union_all(cylinders(data)) if data['kind'] == 'circles' else pockets(lid, data)
  bf = comp.features.baseFeatures.add()
  bf.startEdit()
  comp.bRepBodies.add(acc, bf)
  bf.finishEdit()
  bf.name = piece
  if bf.bodies.count != 1:
    raise RuntimeError('base feature holds %d bodies' % bf.bodies.count)
  b = bf.bodies.item(0)
  b.name = piece
  print('base feature %s, lumps %d, volume mm3 %.1f' % (piece, b.lumps.count, b.volume * 1000))
  return bf


# --- Group --------------------------------------------------------------------

def stage_group(design, first, last, name):
  g = group_named(design, name)
  if g:
    print('group %s exists' % name)
    return g
  tl = design.timeline
  g = tl.timelineGroups.add(first.timelineObject.index, last.timelineObject.index)
  g.name = name
  print('group %s at %d' % (name, g.index))
  return g


def build(name, stop_after=None):
  design, _, comp = design_and_lid()
  data = load(name)
  lid = comp.bRepBodies.itemByName('Body3')
  print('lid volume mm3 %.1f, %s %d, dot volume mm3 %.1f'
        % (lid.volume * 1000, data['kind'], count(data), expected_mm3(data)))
  sk = stage_sketch(comp, lid, name, data)
  if stop_after == 'sketch':
    return
  stage_cut(comp, lid, sk, name, data)
  if stop_after == 'cut':
    return
  dots = stage_dots(comp, lid, name, data)
  if stop_after == 'dots':
    return
  stage_group(design, sk, dots, name)
  print('lid volume after mm3 %.1f, Lid bodies %d' % (lid.volume * 1000, comp.bRepBodies.count))


# --- Checks -------------------------------------------------------------------

def verify(name, overlap=True):
  """Read-only checks with the group active. A polygon inlay is the slab less
  the lid, so it cannot overlap the lid; overlap=False skips that boolean, which
  runs for tens of minutes on hundreds of pieces sitting wall to wall with the cut."""
  _, _, comp = design_and_lid()
  data = load(name)
  want = expected_mm3(data)
  lid = comp.bRepBodies.itemByName('Body3')
  dots = comp.bRepBodies.itemByName(body_name(name, data))
  print('Lid bodies %d' % comp.bRepBodies.count)
  dv, lv = dots.volume * 1000, lid.volume * 1000
  print('dots volume mm3 %.2f, expected %.2f, off %+.3f %%' % (dv, want, 100 * (dv - want) / want))
  print('lid volume mm3 %.2f, drop %.2f, drop minus dots %+.3f'
        % (lv, LID_MM3 - lv, (LID_MM3 - lv) - dv))
  bb = dots.boundingBox
  print('dots bbox z mm %.4f..%.4f, lumps %d' % (bb.minPoint.z * 10, bb.maxPoint.z * 10,
                                               dots.lumps.count))
  if not overlap:
    return
  tmp = adsk.fusion.TemporaryBRepManager.get()
  a, b = tmp.copy(lid), tmp.copy(dots)
  ok = tmp.booleanOperation(a, b, adsk.fusion.BooleanTypes.IntersectionBooleanType)
  print('lid and dots intersection: boolean %s, volume mm3 %.4f, faces %d'
        % (ok, a.volume * 1000 if a.faces.count else 0.0, a.faces.count))


def snapshot(name):
  """Top view of the lid, 1920 x 1520, to build/cane/<name>-lid.png."""
  app = adsk.core.Application.get()
  _, occ, comp = design_and_lid()
  lid = comp.bRepBodies.itemByName('Body3').createForAssemblyContext(occ)
  bb = lid.boundingBox
  cx, cy = (bb.minPoint.x + bb.maxPoint.x) / 2, (bb.minPoint.y + bb.maxPoint.y) / 2
  vp = app.activeViewport
  cam = vp.camera
  cam.isSmoothTransition = False
  cam.cameraType = adsk.core.CameraTypes.OrthographicCameraType
  cam.target = adsk.core.Point3D.create(cx, cy, bb.maxPoint.z)
  cam.eye = adsk.core.Point3D.create(cx, cy, bb.maxPoint.z + 100)
  cam.upVector = adsk.core.Vector3D.create(0, 1, 0)
  cam.isFitView = False
  cam.setExtents((bb.maxPoint.x - bb.minPoint.x) * 1.1, (bb.maxPoint.y - bb.minPoint.y) * 1.1)
  vp.camera = cam
  vp.refresh()
  path = '%s/%s-lid.png' % (JSON_DIR, name.lower())
  print('saved', vp.saveAsImageFile(path, 1920, 1520), path)


def suppress(name):
  design, _, comp = design_and_lid()
  g = group_named(design, name)
  g.isSuppressed = True
  lid = comp.bRepBodies.itemByName('Body3')
  print('group %s suppressed %s, Lid bodies %d, lid volume mm3 %.1f'
        % (name, g.isSuppressed, comp.bRepBodies.count, lid.volume * 1000))


def run(_context):
  build(NAME, STOP_AFTER)
