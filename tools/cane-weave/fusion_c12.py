"""C12 circle inlay on the Lid face, built in Fusion through the MCP script runner.

Reads build/cane/c12-circles.json (from c12_export.py) and adds to the Lid component
one sketch `C12` of every circle on the top face, one extrude `C12 cut` 0.4 mm into
the lid, one base feature `C12 dots` holding the dots as a single body of 2,307 lumps
(a union of temporary cylinders; the slicer wants one body, and a parametric extrude
would make one body per dot), and a timeline group `C12`. Each stage is skipped when
its feature already exists, so the script can be rerun after an MCP timeout.
STOP_AFTER ends a run early.
"""

import json
import math

import adsk.core
import adsk.fusion

JSON_PATH = '/Users/j/Documents/Claude/Projects/cubevox/build/cane/c12-circles.json'
DOC_NAME = 'cubevox-enclosure'
DEPTH_MM = 0.4         # must equal the user parameter inlayDepth
STOP_AFTER = None      # 'sketch' | 'cut' | 'dots' | None (run everything)

# The lid's top face is found by size (the largest flat face looking up); its height
# is read from the model, 13.7662 mm since the owner's window change of 2026-11-03.
FACE_Z_MM = None


def lid_component(design):
  for occ in design.rootComponent.allOccurrences:
    if occ.component.name == 'Lid':
      return occ.component
  raise RuntimeError('no Lid component')


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


def stage_sketch(comp, face, circles):
  sk = feature_named(comp.sketches, 'C12')
  if sk:
    print('sketch C12 exists, curves', sk.sketchCurves.count)
    return sk
  sk = comp.sketches.add(face)
  sk.name = 'C12'
  sk.isComputeDeferred = True
  add = sk.sketchCurves.sketchCircles.addByCenterRadius
  for x, y, r in circles:
    p = sk.modelToSketchSpace(adsk.core.Point3D.create(x / 10, y / 10, FACE_Z_MM / 10))
    p.z = 0
    add(p, r / 10)
  sk.isComputeDeferred = False
  print('sketch C12 drawn, curves', sk.sketchCurves.count, 'profiles', sk.profiles.count)
  return sk


def stage_cut(comp, lid, sk, n_expected):
  ext = comp.features.extrudeFeatures
  f = feature_named(ext, 'C12 cut')
  if f:
    print('extrude C12 cut exists')
    return f
  profs = circle_profiles(sk)
  if profs.count != n_expected:
    raise RuntimeError('circle profiles %d, expected %d' % (profs.count, n_expected))
  before = lid.volume
  inp = ext.createInput(profs, adsk.fusion.FeatureOperations.CutFeatureOperation)
  inp.setDistanceExtent(False, adsk.core.ValueInput.createByString('-inlayDepth'))
  inp.participantBodies = [lid]
  f = ext.add(inp)
  f.name = 'C12 cut'
  print('extrude C12 cut, lid volume mm3 %.1f -> %.1f' % (before * 1000, lid.volume * 1000))
  return f


def dots_body(circles):
  """One temporary body of every dot as a 0.4 mm cylinder, unioned pairwise."""
  tmp = adsk.fusion.TemporaryBRepManager.get()
  zt, zb = FACE_Z_MM / 10, (FACE_Z_MM - DEPTH_MM) / 10
  parts = [tmp.createCylinderOrCone(adsk.core.Point3D.create(x / 10, y / 10, zb), r / 10,
                                    adsk.core.Point3D.create(x / 10, y / 10, zt), r / 10)
           for x, y, r in circles]
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


def stage_dots(comp, circles):
  bf = feature_named(comp.features.baseFeatures, 'C12 dots')
  if bf:
    print('base feature C12 dots exists, lumps', bf.bodies.item(0).lumps.count)
    return bf
  acc = dots_body(circles)
  bf = comp.features.baseFeatures.add()
  bf.startEdit()
  comp.bRepBodies.add(acc, bf)
  bf.finishEdit()
  bf.name = 'C12 dots'
  b = bf.bodies.item(0)
  b.name = 'C12 dots'
  print('base feature C12 dots, lumps', b.lumps.count, 'volume mm3 %.1f' % (b.volume * 1000))
  return bf


def stage_group(design, first, last):
  tl = design.timeline
  for g in tl.timelineGroups:
    if g.name == 'C12':
      print('group C12 exists')
      return g
  g = tl.timelineGroups.add(first.timelineObject.index, last.timelineObject.index)
  g.name = 'C12'
  print('group C12', g.index)
  return g


def run(_context):
  app = adsk.core.Application.get()
  doc = app.activeDocument
  if doc.name.split(' v')[0] != DOC_NAME:
    raise RuntimeError('active document is %s' % doc.name)
  design = adsk.fusion.Design.cast(app.activeProduct)
  with open(JSON_PATH) as fh:
    data = json.load(fh)
  circles = data['circles']
  comp = lid_component(design)
  lid = comp.bRepBodies.itemByName('Body3')
  print('lid volume mm3 %.1f' % (lid.volume * 1000), 'circles', len(circles),
        'dot volume mm3 %.1f' % sum(math.pi * r * r * DEPTH_MM for _, _, r in circles))
  face = top_face(lid)

  sk = stage_sketch(comp, face, circles)
  if STOP_AFTER == 'sketch':
    return
  cut = stage_cut(comp, lid, sk, len(circles))
  if STOP_AFTER == 'cut':
    return
  dots = stage_dots(comp, circles)
  if STOP_AFTER == 'dots':
    return
  stage_group(design, sk, dots)
  print('lid volume after mm3 %.1f' % (lid.volume * 1000), 'Lid bodies', comp.bRepBodies.count)
