# OWL — Licensing, in plain terms

*[🇫🇷 Français](LICENSING.md)*

## In one sentence

OWL is **open source under AGPL-3.0-only**, together with the **OWL Runtime
Exception**. You may use, study, modify and fork it freely, including in a
professional or commercial setting. **The applications you write with OWL stay
under the license of your choice**, including proprietary.

## The model, in five lines

- The **OWL runtime** is under the AGPL.
- **Modifications to the OWL runtime** stay under the AGPL (if you distribute
  them or expose them over a network, you publish your modifications to the
  engine).
- An **independent program** (your `.owl` code, your application) may be
  combined with the OWL runtime and distributed **under the license of your
  choice**.
- The mere fact of combining your program with the runtime — by building it,
  interpreting it, or using OWL's standard library and native functions —
  **does not place** your program under the AGPL.
- This permission **does not allow** relicensing OWL itself or its
  modifications.

## Fixing OWL does not penalize you

You may modify the OWL runtime (fix a bug, improve it), publish those
modifications under the AGPL as the license requires, **and then keep building
and distributing your independent proprietary application** with that modified
runtime. The AGPL obligation bears on **your modifications to the engine**, not
on your application.

## Your obligations when you distribute

### The OWL engine (AGPL)
An executable you distribute contains the OWL runtime (AGPL + Runtime
Exception). The AGPL obligation bears on the corresponding source of the covered
OWL parts — OWL's public source plus any modifications you make to the runtime —
**not** on your independent program. If you expose a modified version of the
engine over a network, AGPL §13 applies to those modifications to the engine.

### Third-party components
OWL embeds third-party libraries, each under its own license (inventory:
`THIRD-PARTY-NOTICES`). In particular, a GUI application that links **Qt
statically** is subject to the specific obligations of the **LGPL-3.0**
(allowing its recipients to relink the application with a modified version of
Qt). This obligation stems from Qt's license — not from OWL — and the Runtime
Exception does not lift it.

## FAQ

**May I write a proprietary / commercial application with OWL?**
Yes. Your application keeps the license of your choice. (For a distributed GUI
app, see the Qt obligation above.)

**Do I have to publish my `.owl` code?**
No. Running or building it with OWL does not place it under the AGPL.

**May I use OWL in a company, internally?**
Yes. In internal use of an unmodified OWL, you do not have to publish your
application code.

**May I fork OWL?**
Yes, as long as your fork respects the AGPL (it stays open). You cannot turn it
into a closed/proprietary version.

**I fixed a bug in OWL. What must I do?**
If you distribute the fixed engine (or expose it modified over a network),
publish your fix under the AGPL. Your application keeps its license.
