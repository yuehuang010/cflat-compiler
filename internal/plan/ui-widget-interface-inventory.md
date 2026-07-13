# UI widget interface inventory (input for Phase 3 of `ui-interface-refactor.md`)

Status: EVIDENCE DOC - read-only survey, no code changed.
Created: 2026-07-13

## What this is

`internal/plan/ui-interface-refactor.md` Phase 3 says "declare `interface IView : IElement`,
`IButton : IElement`, ... for all 23 element classes, carrying the fields each host/reconciler
actually reaches". This doc IS that list: for every one of the 23 element classes, the exact set
of fields and methods reached through a downcast (`X* v = e as X;`) or through a
factory-returned pointer, with `file:line` for every claim, plus a proposed interface
declaration.

Method: `Grep` for ` as (View|Text|...|ComponentElement)\b` across the whole repo, then read the
surrounding code at every hit; plus a sweep of `example/ui/**` for post-construction field
writes through factory pointers (`root.style.gap = 1;`, `prog.tooltip = "...";`). Every class
definition and all 23 `propsEqual` bodies in `cflat/core/ui_native.cb` were read in full.

Real paths (the plan doc omits the `cflat/` prefix):

- `C:\source\cflat-compiler\cflat\core\ui_native.cb` (4081 lines)
- `C:\source\cflat-compiler\cflat\core\ui_native\win32.cb` (3300)
- `C:\source\cflat-compiler\cflat\core\ui_native\winui.cb` (1571)
- `C:\source\cflat-compiler\cflat\core\ui_native\cocoa.cb` (2892)
- `C:\source\cflat-compiler\cflat\core\ui_test.cb` (458)
- `C:\source\cflat-compiler\example\ui\**` (19 `.cb` files in 8 subdirs)

### Real counts vs the plan doc's estimates

| Thing | Plan doc | Measured | Where |
|---|---|---|---|
| downcasts to one of the 23 element classes | 202 | **188** | ui_native.cb 32, win32.cb 52, winui.cb 50, cocoa.cb 52, examples 2, ui_test.cb 0 |
| distinct (widget, field) reach-through pairs | 255 "field touches" | **93 distinct pairs** (many with 3+ call sites each) | see the 23 sections |
| distinct FIELD NAMES reached | ~45 | **45** (exactly) | summary A |
| post-construction field writes in examples | 53 | **63** individual assignments in 11 files | summary, effort table |
| `is` usages | 0 | **0** (confirmed) | - |
| downcasts in `ui_test.cb` | (implied nonzero) | **0** - the file does not name a single element class | grep for the 23 names in `ui_test.cb`: no matches |

Notes on the deltas:

- 188 vs 202: my regex counts ONLY the 23 element classes. The plan's 202 presumably also counted
  `as` casts to non-element classes (e.g. `Window`, `SurfaceCanvas`). The 23-class number is the
  one that matters for Phase 3.
- `ui_native.cb` reports 33 grep hits but one (`:1661`) is prose in a comment ("...as Button"),
  so 32 real casts.
- The plan lists `ui_test.cb` among the files to convert. For the WIDGET interfaces it needs zero
  work: it never touches an element class. (Its `UiTest*` -> `IUiTest` conversion is Phase 6, out
  of scope here.)

---

## 1. View (`ui_native.cb:796`)

Declared fields: `Style style`, `list<Element> children`, `Rect bounds`, `string key`.

| Field | Type | R/W | Sites |
|---|---|---|---|
| `style` | `Style` (NESTED STRUCT) | R | `ui_native.cb:815` (`v.style.width`), `:822` (`v.style.height`), `:851`/`:853` (`v.style.width`), `:1263` (propsEqual, `styleEquals(this.style, o.style)`) |
| `style` | `Style` | W (nested) | `gallery_app.cb:127` `card.style.gap`, `:128` `card.style.backgroundColor`, `:160` `content.style.gap`, `:426` `flexPair.style.gap`; `tui_demo.cb:215` `row.style.flexDirection`, `:291`, `:364`, `:365`, `:388`, `:389`, `:408`, `:409`, `:431`, `:432` (`r.style.gap` / `r.style.flexWrap`); factories `ui_native.cb:2264`, `:2274`, `:2283`, `:3396-3397` |
| `children` | `list<Element>` (OWNING CONTAINER) | R | `example/ui/01-elements/counter.cb:43` - `rootView.children[1] as Button` |

Methods called through a cast: none (`add()` is called on the concrete factory pointer, which
Phase 5 keeps).

Downcasts to View: `ui_native.cb:814`, `:821`, `:850`, `:1262`; `counter.cb:36`. (5)

```cflat
interface IView : IElement
{
    Style style;
    list<Element> children;   // RISK: owning container field, see summary E
    void add(Element child);
};
```

## 2. Text (`ui_native.cb:1272`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `text` | `string` | R | `win32.cb:1291`, `winui.cb:769`, `cocoa.cb:1136` (`setText(h, t.text.data())`); `ui_native.cb:1310` (propsEqual) |
| `tooltip` | `string` | R | `win32.cb:318`, `cocoa.cb:104` (`tooltipOf`) |
| `color` | `int` | W | `win32_shot.cb:31` (`title.color = rgb(33,80,160)`), `:52` (`lbl.color = rgb(150,90,20)`) |

Methods: none through a cast.
Downcasts: `ui_native.cb:1309`, `win32.cb:318`, `:1290`, `winui.cb:769`, `cocoa.cb:104`, `:1135`. (6)

```cflat
interface IText : ITooltipped   // ITooltipped : IElement, see summary A
{
    string text;
    int    color;
};
```

## 3. Button (`ui_native.cb:1322`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `title` | `string` | R | `win32.cb:1280`, `winui.cb:763`, `cocoa.cb:1129`; `ui_native.cb:1423` (propsEqual) |
| `disabled` | `bool` | R | `win32.cb:1281`, `winui.cb:764`, `winui.cb:1327` (`nativeIsEnabled`), `cocoa.cb:1130`; `ui_native.cb:1423` |
| `disabled` | `bool` | W | `win32_settings.cb:76`, `win32_native_settings.cb:67`, `cocoa_native_settings.cb:66`, `winui_demo.cb:38`, `win32_boxes.cb:333`, `todo_app.cb:111`, `:126`, `:132` |
| `backgroundColor` | `int` | R | `win32.cb:1284`, `winui.cb:765`, `cocoa.cb:1247` (`_syncButtonAccent(u64, Button*)`) |
| `color` | `int` | R | `win32.cb:1285`, `winui.cb:766`, `cocoa.cb:1248` |
| `tooltip` | `string` | R | `win32.cb:317`, `cocoa.cb:103` |
| `tooltip` | `string` | W | `win32_native_settings.cb:68` |

`onPress` (`Lambda<void()>`) is NEVER reached as a field from outside the class - every caller goes
through the `fireClick()` method. Keep that pattern.

Methods through a cast: `fireClick()` - `win32.cb:1893`, `winui.cb:918`, `:1297`, `cocoa.cb:1738`,
`counter.cb:50`.
Downcasts: 12 (`win32.cb:317`, `:1279`, `:1892`; `winui.cb:762`, `:917`, `:1296`, `:1327`;
`cocoa.cb:103`, `:1128`, `:1737`; `ui_native.cb:1422`; `counter.cb:43`).

```cflat
interface IButton : IDisableable   // IDisableable : ITooltipped : IElement
{
    string title;
    int    color;
    int    backgroundColor;
    void   fireClick();
};
```

## 4. Box (`ui_native.cb:1436`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `style` | `Style` (NESTED) | R | `ui_native.cb:1532` (propsEqual only) |
| `style` | `Style` | W (nested) | `todo_app.cb:96` `root.style.gap` (root is `Box*`, `todo_app.cb:95`), `winui_demo.cb:24` (`Box*` at `:23`), `win32_settings.cb:32` (`Box*` at `:31`), `win32_native_settings.cb:34`, `cocoa_native_settings.cb:33` |

No host downcasts Box at all. Downcasts: 1 (`ui_native.cb:1531`).
NOTE: `root` in five examples is a `Box*`, not a `View*` - the `root.style.gap = 1` writes belong
to Box, not View.

```cflat
interface IBox : IElement
{
    Style style;
    void  add(Element child);
};
```

## 5. TextInput (`ui_native.cb:1548`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `value` | `string` | R | `win32.cb:1296`, `winui.cb:773`, `cocoa.cb:1141`; `ui_native.cb:1648` (propsEqual) |
| `disabled` | `bool` | R | `win32.cb:1297`, `winui.cb:774`, `:1328`, `cocoa.cb:1142`; `ui_native.cb:1648` |
| `tooltip` | `string` | R | `win32.cb:319`, `cocoa.cb:105` |
| `placeholder` | `string` | W | `win32_settings.cb:41`, `win32_native_settings.cb:43`, `cocoa_native_settings.cb:42` |
| `width` | `int` | W | `win32_settings.cb:40`, `win32_native_settings.cb:42`, `cocoa_native_settings.cb:41`, `win32_boxes.cb:44`, `win32_shot.cb:39`, `tui_demo.cb:50` |
| `onChangeText` | `Lambda<void(string)>` (CLOSURE) | R + INVOKED | `win32.cb:2148` (`ti.onChangeText(nv)`), `winui.cb:1395`, `cocoa.cb:1817` |

Downcasts: 9.

```cflat
interface ITextInput : IDisableable
{
    string value;
    string placeholder;
    int    width;
    Lambda<void(string)> onChangeText;   // RISK: closure field, see summary E
};
```

## 6. Checkbox (`ui_native.cb:1662`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `label` | `string` | R | `win32.cb:1302`, `winui.cb:779`, `cocoa.cb:1147`; `ui_native.cb:1738` |
| `checked` | `bool` | R | `win32.cb:1303`, `winui.cb:780`, `:1313` (`nativeIsChecked`), `cocoa.cb:1148`; `ui_native.cb:1738` |
| `disabled` | `bool` | R | `win32.cb:1304`, `winui.cb:781`, `:1329`, `cocoa.cb:1149`; `ui_native.cb:1739` |
| `tooltip` | `string` | R | `win32.cb:320`, `cocoa.cb:106` |

`onChange` (`Lambda<void(bool)>`) reached only through `fireToggle()`.
Methods: `fireToggle()` - `win32.cb:1895`, `winui.cb:920`, `:1299`, `cocoa.cb:1740`.
Downcasts: 12.

```cflat
interface ICheckbox : IDisableable
{
    string label;
    bool   checked;
    void   fireToggle();
};
```

## 7. ProgressBar (`ui_native.cb:1750`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `value` | `int` | R | `win32.cb:1310`, `winui.cb:787`, `cocoa.cb:1155`; `ui_native.cb:1797` |
| `width` | `int` | R | `ui_native.cb:1797` (propsEqual only) |
| `tooltip` | `string` | R | `win32.cb:321`, `cocoa.cb:107` |
| `tooltip` | `string` | W | `gallery_app.cb:245` (`prog.tooltip = "download progress"`) |

ProgressBar has NO `disabled` field (`ui_native.cb:1750-1757`) - it must not sit under
`IDisableable`.
Downcasts: 6.

```cflat
interface IProgressBar : ITooltipped
{
    int value;
    int width;
};
```

## 8. Slider (`ui_native.cb:1809`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `value` | `int` | R | `win32.cb:1316`, `winui.cb:793`, `cocoa.cb:1161`; `ui_native.cb:1909` |
| `disabled` | `bool` | R | `win32.cb:1317`, `winui.cb:794`, `:1331`, `cocoa.cb:1162`; `ui_native.cb:1909` |
| `tooltip` | `string` | R | `win32.cb:322`, `cocoa.cb:108` |

`onChange` (`Lambda<void(int)>`) reached only through `fireSet(int)`.
Methods: `fireSet(int)` - `win32.cb:2306`, `winui.cb:1534`, `cocoa.cb:1840`.
Downcasts: 9.

```cflat
interface ISlider : IDisableable
{
    int  value;
    int  width;
    void fireSet(int v);
};
```
(`width` is not currently reached externally on Slider; included only if the factory ergonomics
want it. Drop it if you want a strictly evidence-driven contract.)

## 9. TextArea (`ui_native.cb:1924`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `value` | `string` | R | `win32.cb:1146` (creation-time seed), `winui.cb:732`, `cocoa.cb:1011` |
| `disabled` | `bool` | R | `win32.cb:1325`, `winui.cb:796`, `:1332`, `cocoa.cb:1170`; `ui_native.cb:1997` |
| `tooltip` | `string` | R | `win32.cb:323`, `cocoa.cb:109` |
| `tooltip` | `string` | W | `fedit.cb:271` (`ed.tooltip = "Editor - type your text here"`) |
| `onChange` | `Lambda<void()>` (CLOSURE) | R + INVOKED | `win32.cb:2157` (`ta.onChange()`), `cocoa.cb:1826` |

Downcasts: 9.

```cflat
interface ITextArea : IDisableable
{
    string value;
    Lambda<void()> onChange;   // RISK: closure field
};
```

## 10. StatusBar (`ui_native.cb:2019`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `parts` | `list<string>` (OWNING CONTAINER) | R | `win32.cb:1263`, `:1266` (`_syncStatusBar(u64, StatusBar*)`), `winui.cb:802`, `cocoa.cb:1177-1181`; `ui_native.cb:2074`, `:2078` (propsEqual: count + element-wise compare) |

No tooltip field, no disabled field. Downcasts: 4 (`ui_native.cb:2072`, `win32.cb:1329`,
`winui.cb:799`, `cocoa.cb:1174`).

```cflat
interface IStatusBar : IElement
{
    list<string> parts;   // RISK: owning container field
    void addPart(string s);
};
```

## 11. ScrollView (`ui_native.cb:2107`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `style` | `Style` (NESTED) | R | `ui_native.cb:2251` (propsEqual only) |
| `contentH` | `int` | R | `cocoa.cb:1096` (`chDip = svn.contentH`) - **COCOA ONLY** |

Downcasts: 2 (`ui_native.cb:2250`, `cocoa.cb:1094`).

```cflat
interface IScrollView : IElement
{
    Style style;
    int   contentH;    // only cocoa.cb:1096 reads it - keep it, the interface is the UNION
    void  add(Element child);
};
```

## 12. RadioButton (`ui_native.cb:2361`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `label` | `string` | R | `win32.cb:1335`, `winui.cb:808`, `cocoa.cb:1188`; `ui_native.cb:2408` |
| `checked` | `bool` | R | `win32.cb:1336`, `winui.cb:809`, `:1314`, `cocoa.cb:1189`; `ui_native.cb:2408` |
| `disabled` | `bool` | R | `win32.cb:1337`, `winui.cb:810`, `:1330`, `cocoa.cb:1190`; `ui_native.cb:2409`, `:2492` |
| `index` | `int` | R | `win32.cb:1906`, `winui.cb:925`, `:1304`, `cocoa.cb:1751`; `ui_native.cb:2495` |
| `groupFirst` | `bool` | R | `win32.cb:1174` (WS_GROUP) |
| `groupFirst` | `bool` | W | `ui_native.cb:2462` (`rb.groupFirst = i == 0` in `RadioGroup.layout`) |
| `bounds` | `Rect` (NESTED) | R | `ui_native.cb:2492` (`rb.bounds.contains(e.x, e.y)`) - could use `nodeBounds()` instead |
| `tooltip` | `string` | R | `win32.cb:324`, `cocoa.cb:110` |

Downcasts: 11.

```cflat
interface IRadioButton : IDisableable
{
    string label;
    bool   checked;
    int    index;
    bool   groupFirst;
};
```
(`bounds` intentionally omitted: `IElement.nodeBounds()` already covers `ui_native.cb:2492`.)

## 13. RadioGroup (`ui_native.cb:2424`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `onChange` | `Lambda<void(int)>` (CLOSURE) | R + INVOKED | `win32.cb:1906` (`g.onChange(rb.index)`), `winui.cb:925`, `:1304`, `cocoa.cb:1751` |
| `value` | `int` | R | `ui_native.cb:2533` (propsEqual) |
| `style` | `Style` (NESTED) | R | `ui_native.cb:2533` (propsEqual) |

Downcasts: 5.

```cflat
interface IRadioGroup : IElement
{
    int   value;
    Style style;
    Lambda<void(int)> onChange;   // RISK: closure field
    void  add(Element child);
    void  addOption(string label);
};
```

## 14. ComboBox (`ui_native.cb:2545`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `items` | `list<string>` (OWNING CONTAINER) | R | `win32.cb:1408`, `:1412`, `:1414` (`_syncCombo(u64, ComboBox*)`), `winui.cb:621`, `:625`, `:627`, `cocoa.cb:1258`, `:1262`, `:1264`; `ui_native.cb:2635`, `:2639` |
| `selectedIndex` | `int` | R | `win32.cb:1419`, `:1420`, `winui.cb:634`, `cocoa.cb:1269`, `:1775`; `ui_native.cb:2634` |
| `disabled` | `bool` | R | `win32.cb:1343`, `winui.cb:816`, `:1333`, `cocoa.cb:1196`; `ui_native.cb:2634` |
| `tooltip` | `string` | R | `win32.cb:325`, `cocoa.cb:111` |
| `width` | `int` | W | `gallery_app.cb:116` (`cb.width = 10`) |
| `onChange` | `Lambda<void(int)>` (CLOSURE) | R + INVOKED | `win32.cb:1931`, `winui.cb:1383`, `cocoa.cb:1775` |

Downcasts: 9.

```cflat
interface IComboBox : IDisableable
{
    list<string> items;          // RISK: owning container field
    int    selectedIndex;
    int    width;
    Lambda<void(int)> onChange;  // RISK: closure field
    void   addItem(string s);
};
```

## 15. ListView (`ui_native.cb:2659`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `columns` | `list<string>` | R | `win32.cb:1157`, `:1161`, `cocoa.cb:1020`, `:1024`; `ui_native.cb:2769` (count only) |
| `colWidths` | `list<int>` | R | `win32.cb:1160`, `cocoa.cb:1023` |
| `multiSelect` | `bool` | R | `win32.cb:1164`; `ui_native.cb:2768` |
| `rowText` | `Lambda<string(int,int)>` (CLOSURE) | R | `win32.cb:1167`, `winui.cb:739`, `cocoa.cb:1029`-ish (all: `_boxListRow(lv.rowText)`) |
| `rowText` | | W | `gallery_app.cb:317`, `todo_app.cb:120` |
| `rowCount` | `int` | R | `win32.cb:1350`, `winui.cb:821`, `cocoa.cb:1203`; `ui_native.cb:2767` |
| `selectedIndex` | `int` | R | `win32.cb:1351`, `:1973`, `winui.cb:822`, `cocoa.cb:1204`, `:1524`, `:2113`; `ui_native.cb:2767` |
| `disabled` | `bool` | R | `win32.cb:1353`, `winui.cb:823`, `:1334`, `cocoa.cb:1205`; `ui_native.cb:2768` |
| `onSelect` | `Lambda<void(int)>` (CLOSURE) | INVOKED | `win32.cb:1973`, `winui.cb:1419`, `cocoa.cb:1524`, `:2113` |
| `onActivate` | `Lambda<void(int)>` (CLOSURE) | INVOKED | `win32.cb:1986`, `winui.cb:1431`, `cocoa.cb:1538`, `:2133` |
| `tooltip` | `string` | R | `win32.cb:326`, `cocoa.cb:112` |

ListView is the single heaviest widget: 10 reached fields, 3 of them closures, 2 of them lists.
Downcasts: 15.

```cflat
interface IListView : IDisableable
{
    list<string> columns;      // RISK: owning container
    list<int>    colWidths;    // RISK: owning container
    int          rowCount;
    int          selectedIndex;
    bool         multiSelect;
    Lambda<string(int,int)> rowText;      // RISK: closure
    Lambda<void(int)>       onSelect;     // RISK: closure
    Lambda<void(int)>       onActivate;   // RISK: closure
    void addColumn(string title, int widthDip);
};
```

## 16. TabPane (`ui_native.cb:2846`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `title` | `string` | R | `ui_native.cb:2937` (`TabControl._title`), `:2903` (propsEqual) |

No host reaches TabPane at all (it is not native-mapped).
Downcasts: 2 (`ui_native.cb:2902`, `:2935`).

```cflat
interface ITabPane : IElement
{
    string title;
    void   add(Element child);
};
```

## 17. TabControl (`ui_native.cb:2916`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `selectedTab` | `int` | R | `win32.cb:1232`, `:930` (`_syncTabs`), `:2040`, `winui.cb:701`, `:1440`, `cocoa.cb:1062`, `:1288`, `:1789`; `ui_native.cb:3036` |
| `selectedTab` | `int` | W | `ui_native.cb:3287` (`adoptContainerProps`: `a.selectedTab = b.selectedTab`) |
| `children` | `list<Element>` | R | `win32.cb:919`, `:923`, `cocoa.cb:1277`, `:1281`; `ui_native.cb:3037` (count only) |
| `disabled` | `bool` | R | `win32.cb:1360`, `winui.cb:825`, `:1335`, `cocoa.cb` (not synced); |
| `tooltip` | `string` | R | `win32.cb:327`, `cocoa.cb:113` |
| `width`, `height` | `int` | W | `fedit.cb:253` (`tabs.width = 100; tabs.height = 3;`) |
| `onSelectTab` | `Lambda<void(int)>` (CLOSURE) | INVOKED | `win32.cb:2040`, `winui.cb:1454`, `cocoa.cb:1789` |

Method through a cast: `_title(int)` - `win32.cb:925`, `cocoa.cb:1283` (a private-by-convention
method; it must be on the interface if the hosts keep calling it).
Downcasts: 12.

```cflat
interface ITabControl : IDisableable
{
    list<Element> children;   // RISK: owning container
    int    selectedTab;
    int    width;
    int    height;
    Lambda<void(int)> onSelectTab;   // RISK: closure
    string _title(int i);
    void   addPane(TabPane* p);      // NOTE: still a concrete ptr; see Phase 5
};
```

## 18. TreeView (`ui_native.cb:3054`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `childCount` | `Lambda<int(int)>` (CLOSURE) | R | `win32.cb:1187`, `winui.cb:745`, `cocoa.cb:1035` (all `_boxTree(tv.childCount, tv.childId, tv.label)`) |
| `childId` | `Lambda<int(int,int)>` (CLOSURE) | R | same three lines |
| `label` | `Lambda<string(int)>` (CLOSURE) | R | same three lines |
| `childCount`/`childId`/`label` | | W | `fedit.cb:239`, `:240`, `:241`; `gallery_app.cb:347`, `:352`, `:357` |
| `selectedNode` | `int` | R | `win32.cb:2057`, `cocoa.cb:1803`; `ui_native.cb:3129` |
| `disabled` | `bool` | R | `win32.cb:1368`, `winui.cb:826`, `:1336`, `cocoa.cb:1217`; `ui_native.cb:3129` |
| `onSelect` | `Lambda<void(int)>` (CLOSURE) | INVOKED | `win32.cb:2057`, `winui.cb:1490`, `cocoa.cb:1803` |
| `onExpand` | `Lambda<void(int)>` (CLOSURE) | INVOKED | `win32.cb:2075`, `winui.cb:1482`, `cocoa.cb:1613` |
| `tooltip` | `string` | R | `win32.cb:328`, `cocoa.cb:114` |
| `tooltip` | `string` | W | `fedit.cb:242` |

5 closure fields - the worst case in the whole framework.
Downcasts: 12.

```cflat
interface ITreeView : IDisableable
{
    Lambda<int(int)>     childCount;   // RISK: closure
    Lambda<int(int,int)> childId;      // RISK: closure
    Lambda<string(int)>  label;        // RISK: closure
    int  selectedNode;
    Lambda<void(int)> onSelect;        // RISK: closure
    Lambda<void(int)> onExpand;        // RISK: closure
};
```

## 19. SplitView (`ui_native.cb:3146`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `dividerBounds` | `Rect` (NESTED) | R | `win32.cb:1616` (`Rect d = sv.dividerBounds;` - the gutter hit-test) |
| `ratio` | `int` | R/W | R: `ui_native.cb:3268` (propsEqual); W: `:3293` (`adoptContainerProps`) |
| `vertical` | `bool` | R/W | same two lines (`:3268`, `:3293`) |
| `style` | `Style` (NESTED) | R | `ui_native.cb:3269` |
| `style` | `Style` | W (nested) | `fedit.cb:230` (`split.style.width = hostWidth(); split.style.height = splitH;`) |
| `onRatioChange` | `Lambda<void(int)>` (CLOSURE) | R + INVOKED | `win32.cb:2087`, `:2216`, `winui.cb:1506`, `cocoa.cb:1993`; W: `fedit.cb:231` |

Method through a cast: `_ratioAt(int, int)` - `win32.cb:2087`, `:2216`, `winui.cb:1506`,
`cocoa.cb:1993`.
Downcasts: 7.

```cflat
interface ISplitView : IElement
{
    int   ratio;
    bool  vertical;
    Style style;
    Rect  dividerBounds;
    Lambda<void(int)> onRatioChange;   // RISK: closure
    int   _ratioAt(int px, int py);
    void  add(Element child);
};
```

## 20. Image (`ui_native.cb:3410`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `pixels` | `u64` | R | `win32.cb:1376`, `:1378`, `:1380`, `:1381`, `winui.cb:830`, `:831`, `cocoa.cb:1224`, `:1226`; `ui_native.cb:3456` |
| `pixels` | `u64` | W | `gallery_app.cb:184` |
| `pxW` | `int` | R | `win32.cb:1380`, `winui.cb:831`, `cocoa.cb:1226`; `ui_native.cb:3456` |
| `pxH` | `int` | R | same |
| `pxW`/`pxH` | `int` | W | `gallery_app.cb:184` |
| `source` | `string` | R | `win32.cb:1384`, `:1386` - **WIN32 ONLY** (no cocoa/winui `.bmp` path) |
| `width`, `height` | `int` | R | `ui_native.cb:3457` (propsEqual) |
| `tooltip` | `string` | R | `win32.cb:329`, `cocoa.cb:115` |

`altText` is used only inside `Image.paint` (`ui_native.cb:3443`) - never reached externally.
Downcasts: 6.

```cflat
interface IImage : ITooltipped
{
    u64    pixels;
    int    pxW;
    int    pxH;
    int    width;
    int    height;
    string source;    // win32.cb:1384 only - union rule
};
```

## 21. GroupBox (`ui_native.cb:3480`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `title` | `string` | R | `win32.cb:1393`, `winui.cb:833`, `cocoa.cb:1233`; `ui_native.cb:3555` |
| `style` | `Style` (NESTED) | R | `ui_native.cb:3555` |
| `style` | `Style` | W (nested) | `gallery_app.cb:262` (`grp.style.width = 30; grp.style.height = 4;`) |
| `tooltip` | `string` | R | `win32.cb:330`, `cocoa.cb:116` |

Downcasts: 6.

```cflat
interface IGroupBox : ITooltipped
{
    string title;
    Style  style;
    void   add(Element child);
};
```

## 22. CanvasView (`ui_native.cb:3580`)

| Field | Type | R/W | Sites |
|---|---|---|---|
| `onPaint` | `Lambda<void(Canvas)>` (CLOSURE) | R | `win32.cb:1196` (`_boxCanvas(cvw.onPaint)`), `winui.cb:753`, `cocoa.cb:1043` |
| `onPaint` | | W | `gallery_app.cb:194` |
| `tooltip` | `string` | R | `win32.cb:331`, `cocoa.cb:117` |

`propsEqual` (`ui_native.cb:3610`) compares NO fields - it is a bare null-check.
Downcasts: 6.

```cflat
interface ICanvasView : ITooltipped
{
    int    width;
    int    height;
    Lambda<void(Canvas)> onPaint;   // RISK: closure carrying an interface param
};
```
(`width`/`height` are set only via JSX/factory today; include them only if Phase 5 wants
post-construction sizing.)

## 23. ComponentElement (`ui_native.cb:3885`)

**ZERO downcasts anywhere in the repo.** `propsEqual` is `return true;` (`ui_native.cb:3952`); no
field of it is ever reached from outside the class. `mount()` (`ui_native.cb:3963`) returns
`ComponentElement*` but callers immediately treat it as an `Element`.

```cflat
// NO INTERFACE NEEDED. Everything callers do with a ComponentElement is already on IElement.
// If Phase 5 changes mount()'s return type, `move IElement mount(...)` is sufficient.
```

---

## Summary / design answers

### A. Distinct fields, and what to hoist

**45 distinct field names** are reached (matching the plan's ~45 estimate), across **93 distinct
(widget, field) pairs**. The full 45: `style, children, text, color, tooltip, title, disabled,
backgroundColor, value, placeholder, width, onChangeText, label, checked, parts, contentH, index,
groupFirst, bounds, onChange, items, selectedIndex, columns, colWidths, rowCount, rowText,
multiSelect, onSelect, onActivate, selectedTab, onSelectTab, height, childCount, childId,
selectedNode, onExpand, ratio, vertical, onRatioChange, dividerBounds, pixels, pxW, pxH, source,
onPaint`.

Sharing (verified against every class definition):

| Candidate | Classes that HAVE it | Verdict |
|---|---|---|
| `key` (`string`) | ALL 23 | Already exposed as `keyOf()` / `setKey()` on `Element`. Do NOT add as a field. |
| `bounds` (`Rect`) | ALL 23 | Already exposed as `nodeBounds()`. Do NOT add as a field (rewrite `ui_native.cb:2492` to use it). |
| `tooltip` (`string`) | 15: Text, Button, TextInput, Checkbox, ProgressBar, Slider, TextArea, RadioButton, ComboBox, ListView, TabControl, TreeView, Image, GroupBox, CanvasView. **MISSING** on View (`:796`), Box (`:1436`), ScrollView (`:2107`), RadioGroup (`:2424`), StatusBar (`:2019`), TabPane (`:2846`), ComponentElement (`:3885`) | **REJECT hoisting to IElement** (7 classes lack it). **RECOMMEND an intermediate `ITooltipped : IElement { string tooltip; }`** implemented by exactly those 15. |
| `disabled` (`bool`) | 10: Button, TextInput, Checkbox, Slider, TextArea, RadioButton, ComboBox, ListView, TabControl, TreeView. **MISSING** on ProgressBar, Text, View, Box, StatusBar, ScrollView, RadioGroup, TabPane, Image, GroupBox, CanvasView, ComponentElement | **REJECT hoisting to IElement.** All 10 disableable classes ALSO have `tooltip`, so a single linear chain works: `IDisableable : ITooltipped { bool disabled; }`. |
| `style` (`Style`) | 6: View, Box, ScrollView, RadioGroup, SplitView, GroupBox | Not worth an intermediate - repeat it in the 6 interfaces (a shared `IStyled` would fight the `ITooltipped`/`IDisableable` chain, since GroupBox needs both). |
| `title`/`text`/`label`/`value` | Type-CONFLICTING: `value` is `string` on TextInput/TextArea but `int` on ProgressBar/Slider/RadioGroup; `label` is `string` on Checkbox/RadioButton but `Lambda<string(int)>` on TreeView (`:3058`) | **REJECT any hoist.** These are same-name/different-type; a shared slot is impossible. |
| `color`, `backgroundColor` | color: 6 (Text, Button, TextInput, Checkbox, ProgressBar, TextArea); bg: 3 (Button, TextInput, TextArea) - and only Button's are ever reached | Repeat where needed. Not worth an interface. |

**Concrete recommendation:**

```cflat
interface ITooltipped : IElement { string tooltip; };            // 15 implementors
interface IDisableable : ITooltipped { bool disabled; };          // 10 implementors
// then: IButton : IDisableable, IText : ITooltipped, IView : IElement, ...
```

Payoff, measured: `tooltipOf()` is a 15-way `as`-chain duplicated in TWO hosts
(`win32.cb:317-331`, `cocoa.cb:103-117`) = 30 downcasts that collapse to ONE
`ITooltipped t = node as ITooltipped;`. `nativeIsEnabled` in `winui.cb:1327-1336` is a 10-way
chain that collapses to one. That is 40 of the 188 casts (21%) removed by two interfaces.

Caveat to verify in Phase 1: this needs a TWO-LEVEL derived->parent upcast
(`IButton` -> `ITooltipped` -> `IElement`). The spike only proved a one-level upcast, and the plan
notes the vtable-prefix property is already rejected as the mechanism. If multi-level upcast is not
wired, fall back to declaring `tooltip`/`disabled` directly on each of the 15/10 interfaces (costs
nothing but repetition; the 40-cast collapse is then lost).

### B. Do the 3 hosts reach the same fields?

**No. Each host reaches a different subset; the interface must be the UNION.** Concretely:

| Field | win32 | winui | cocoa | Note |
|---|---|---|---|---|
| `tooltip` (all 15 widgets) | YES (`win32.cb:317-331`) | **NO - winui.cb has ZERO `.tooltip` reads** (grep: no matches) | YES (`cocoa.cb:103-117`) | winui rides `ToolTipService` and returns `nativeTooltipCount() = 0` (`winui.cb:1343`) |
| `disabled` via `nativeIsEnabled` | NO - queries the OS (`IsWindowEnabled`, `win32.cb:2804`) | **YES - winui is the ONLY host that reads the element model** (`winui.cb:1327-1336`, 10 casts) | NO - queries the OS (`msgBool1(..., "isEnabled")`, `cocoa.cb:2169`) | |
| `checked` via `nativeIsChecked` | NO - `BM_GETCHECK` (`win32.cb:2795`) | **YES, winui only** (`winui.cb:1313`, `:1314`) | NO - `"state"` (`cocoa.cb:2160`) | |
| `ScrollView.contentH` | NO | NO | **YES, cocoa only** (`cocoa.cb:1096`) | sizes the NSScrollView document view |
| `Image.source` | **YES, win32 only** (`win32.cb:1384`, `:1386`) | NO | NO | the `.bmp` file path |
| `RadioButton.groupFirst` | **YES, win32 only** (`win32.cb:1174`) | NO | NO | WS_GROUP |
| `ListView.columns` / `colWidths` / `multiSelect` | YES (`win32.cb:1157-1164`) | columns/colWidths: NO; multiSelect: NO | columns/colWidths: YES (`cocoa.cb:1020-1024`); multiSelect: NO | winui's list is model-backed only |
| `TabControl.children` (via `_syncTabs`) | YES (`win32.cb:919`) | NO | YES (`cocoa.cb:1277`) | |
| `TabControl._title(i)` | YES (`win32.cb:925`) | NO | YES (`cocoa.cb:1283`) | |
| `StatusBar.parts` | YES | YES | YES | the one data field all three read |
| `Button.title` / `disabled` / `color` / `backgroundColor` | YES | YES | YES | |

Single-host fields to keep in the union anyway: `ScrollView.contentH` (cocoa), `Image.source`
(win32), `RadioButton.groupFirst` (win32). Also note `SplitView.dividerBounds` is read only by
win32 (`win32.cb:1616`) - cocoa/winui hit-test the gutter differently.

### C. Can `propsEqual` be simplified or generated?

**Simplified only marginally; NOT generatable. Do not promise a generator.** Evidence from reading
all 23 bodies:

- 5 of 23 are NOT a field compare at all: `TextArea` deliberately IGNORES `value`
  (`ui_native.cb:1994-1998` - "the value flowing down is a PUSH... that would clobber the native
  buffer mid-edit"); `CanvasView` compares nothing (`:3610`); `ComponentElement` returns `true`
  (`:3952`); `ListView` compares `columns.count()` but NOT the column contents and NONE of its 3
  closures (`:2765-2770`); `Image` compares `pixels/pxW/pxH/width/height` but deliberately NOT
  `source`/`altText`/`tooltip` (`:3453-3458`).
- 2 do a DEEP element-wise list compare with a loop: `StatusBar` (`:2070-2082`), `ComboBox`
  (`:2630-2643`).
- 3 delegate to `styleEquals()` on a nested struct: `View` (`:1263`), `Box` (`:1532`),
  `ScrollView` (`:2251`), plus `RadioGroup` (`:2533`), `SplitView` (`:3269`), `GroupBox` (`:3555`).
- NO closure field is ever compared anywhere - by design (closures are re-boxed each render).

So the compare set is a hand-picked subset of the field set, with per-widget semantics that the
comments explicitly justify. A generator over "all interface fields" would be WRONG for at least
TextArea, ListView, and Image.

What Phase 3 actually buys here: the `X* o = other as X; return o != nullptr && ...` prologue
becomes `IX o = other as IX; return o != nullptr && ...` (needs F3, interface null-compare). That
is a rename, not a saving. **Budget 23 x ~2 lines of churn and zero deletions.** The real cast
savings are in the hosts (question A), not in `propsEqual`.

### D. Widgets needing NO new interface

- **ComponentElement** - definitively: zero downcasts repo-wide, `propsEqual` is `return true`, no
  field reached. `IElement` already covers 100% of its use.

Everything else reaches at least one field. The near-misses, for scoping:

- **TabPane** - one field (`title`), reached only inside `ui_native.cb` (`:2903`, `:2937`).
- **Box** - one field (`style`), one `propsEqual` cast (`:1531`) plus 5 example writes.
- **StatusBar** - one field (`parts`).
- **ScrollView** - two fields (`style`, `contentH`).

No widget is method-only.

### E. Fields that are awkward or dangerous as interface fields

**E1. Closure fields (14 of the 45) - the highest risk.** Widgets and fields:
`TextInput.onChangeText`, `TextArea.onChange`, `RadioGroup.onChange`, `ComboBox.onChange`,
`ListView.rowText`/`onSelect`/`onActivate`, `TreeView.childCount`/`childId`/`label`/`onSelect`/
`onExpand`, `SplitView.onRatioChange`, `CanvasView.onPaint`.

Risk detail: `Lambda<T>` is an OWNING value type with a heap env (per the lambda Option-A design;
the env pointer carries an OWNED tag in its low bit). Taking its byte offset is fine - the field IS
a fat/owning value stored inline in the class, so `dataPtr + offset` names a valid lvalue of type
`Lambda<...>`. Two concrete hazards:

1. **Read semantics.** `_boxListRow(lv.rowText)` (`win32.cb:1167`) passes the field BY VALUE.
   Today lambdas are CLONE-by-default at a by-value arg, so the field survives. If the
   interface-field read path is instead routed through the generic move-on-read rule (as owning
   `string`/container locals are), the field is NULLED and the second render invokes a freed
   closure. The Phase-1 F1 work must prove `IListView.rowText` passed as a by-value arg CLONES,
   with a HeapAudit-clean gallery run as the gate.
2. **Write semantics.** `gallery_app.cb:317` (`lv.rowText = (int r, int c) => {...}`) and
   `fedit.cb:239-241` overwrite a closure field. That is exactly the destruct-before-overwrite path
   the spike found broken for interface fields (byte-GEP vs 2-index-GEP sniffing). It was fixed for
   `string` via `IsInterfaceField`; verify the same fix covers `Lambda` (a leak here is silent).

Mitigation available today, and RECOMMENDED where it fits: Button/Checkbox/Slider already keep
their closure OFF the reach-through surface by exposing `fireClick()` / `fireToggle()` /
`fireSet(int)` (`ui_native.cb:1333`, `:1673`, `:1819`). Adding the same wrapper methods
(`fireChangeText(string)`, `fireSelect(int)`, `fireRatioChange(int)`, ...) would remove 9 of the 14
closure fields from the interfaces entirely, leaving only the four that are genuinely READ as
values to be boxed (`ListView.rowText`, `TreeView.childCount`/`childId`/`label`,
`CanvasView.onPaint`) plus the 6 example WRITE sites. Strongly consider this before shipping
closure interface fields.

**E2. Owning container fields (7).** `View.children`/`Box`(via childList)/`TabControl.children`
(`list<Element>`), `StatusBar.parts`/`ComboBox.items`/`ListView.columns` (`list<string>`),
`ListView.colWidths` (`list<int>`). Same lvalue argument applies - `cb.items.count()` must resolve
to a method call ON the lvalue, not on a COPY of the list. If the interface-field read ever
materializes a temporary `list<T>` value, it will either leak or double-free on scope exit (and
`list<Element>` additionally does not destruct its interface elements, `core/list.cb:244`). This is
the top correctness gate for question E after closures.

**E3. Nested struct fields (`Style`, `Rect`).** `View`/`Box`/`ScrollView`/`RadioGroup`/`SplitView`/
`GroupBox`.`style`, `SplitView.dividerBounds`, `RadioButton.bounds`. The spike explicitly proved
`b.style.gap = 1` works against two implementors with different offsets, so these are the LOW-risk
ones. `RadioButton.bounds` (`ui_native.cb:2492`) should be rewritten to `nodeBounds()` rather than
becoming an interface field.

**E4. Owning `string` fields (title/text/value/label/placeholder/tooltip/source).** Reads are all of
the form `"" + b.tooltip` (`win32.cb:317`) or `b.title.data()` (`win32.cb:1280`), i.e. copy /
borrow - safe. Writes exist in examples (`save.tooltip = "Save these settings"`,
`win32_native_settings.cb:68`; `nm.placeholder = "your name"`, `:43`) and become interface-field
writes after Phase 5 - the exact leak the spike's `IsInterfaceField` fix addresses. Keep
`example.bat` leak-clean as the gate.

**E5. Type-name collisions across widgets.** `value` is `string` (TextInput, TextArea) and `int`
(ProgressBar, Slider, RadioGroup); `label` is `string` (Checkbox, RadioButton) and
`Lambda<string(int)>` (TreeView). Harmless in the per-widget interfaces, but it kills any idea of a
shared `IValued`/`ILabeled`.

---

## Per-widget effort estimate

Lines are "lines that must be edited", counting a downcast site as 1-2 lines (the cast + the null
guard) and each field write as 1 line. "Framework" = `cflat/core/ui_native.cb`. "Hosts" =
`win32.cb` + `winui.cb` + `cocoa.cb` combined. "Examples" = `example/ui/**`.

| Widget | Casts (fw / hosts / ex) | Fields reached | Framework | Hosts | Examples | Notes |
|---|---|---|---|---|---|---|
| View | 4 / 0 / 1 | 2 | ~14 (3 flex probes + propsEqual + 4 factories) | 0 | ~15 | flex probes reach a NESTED struct |
| Text | 1 / 5 / 0 | 3 | ~3 | ~8 | 2 | |
| Button | 1 / 10 / 1 | 5 | ~3 | ~18 | 8 | most-touched leaf |
| Box | 1 / 0 / 0 | 1 | ~3 | 0 | 5 | `root` in 5 examples is a Box |
| TextInput | 1 / 8 / 0 | 6 | ~3 | ~14 | 9 | 1 closure |
| Checkbox | 1 / 11 / 0 | 4 | ~4 | ~18 | 0 | |
| ProgressBar | 1 / 5 / 0 | 3 | ~3 | ~8 | 1 | no `disabled` |
| Slider | 1 / 8 / 0 | 3 | ~3 | ~12 | 0 | |
| TextArea | 1 / 8 / 0 | 4 | ~3 | ~13 | 1 | 1 closure |
| StatusBar | 1 / 3 / 0 | 1 | ~12 (deep list compare) | ~9 | 0 | `list<string>` |
| ScrollView | 1 / 1 / 0 | 2 | ~3 | ~3 | 0 | `contentH` cocoa-only |
| RadioButton | 3 / 8 / 0 | 7 | ~8 | ~14 | 0 | `groupFirst` win32-only |
| RadioGroup | 1 / 4 / 0 | 3 | ~4 | ~5 | 0 | 1 closure |
| ComboBox | 1 / 8 / 0 | 6 | ~14 (deep list compare) | ~20 | 1 | list + closure |
| ListView | 1 / 14 / 0 | 10 | ~7 | ~32 | 2 | **heaviest**: 3 closures + 2 lists |
| TabPane | 2 / 0 / 0 | 1 | ~5 | 0 | 0 | never native-mapped |
| TabControl | 3 / 9 / 0 | 7 | ~8 | ~22 | 2 | `adoptContainerProps` writes |
| TreeView | 1 / 11 / 0 | 8 | ~4 | ~20 | 7 | **5 closures** |
| SplitView | 3 / 4 / 0 | 6 | ~9 | ~10 | 3 | `adoptContainerProps` writes |
| Image | 1 / 5 / 0 | 7 | ~4 | ~14 | 3 | `source` win32-only |
| GroupBox | 1 / 5 / 0 | 3 | ~4 | ~8 | 2 | |
| CanvasView | 1 / 5 / 0 | 2 | ~2 | ~8 | 1 | 1 closure w/ Canvas param |
| ComponentElement | 0 / 0 / 0 | 0 | 0 | 0 | 0 | **no interface needed** |
| **Total** | **32 / 156 / 2** (188 minus the 32 fw = 156... see note) | **93 pairs / 45 names** | **~123** | **~256** | **~62** | plus ~25 lines of new `interface` declarations x 22 |

Note on the cast columns: hosts total 154 (52 + 50 + 52), examples 2, framework 32 = 188. The
per-row host figures above sum to slightly more than 154 because the `tooltipOf` chains
(`win32.cb:317-331`, `cocoa.cb:103-117`) are attributed to each widget row; they are 30 casts that
COLLAPSE TO 2 under the `ITooltipped` recommendation in A.

Aggregate: **~440 lines touched** across framework + hosts + examples for Phase 3 + Phase 4, versus
the plan's "~1,190 lines across the framework" (which also covers Phases 2, 5, 6, 7 - the rename,
factories, contexts and docs). The two numbers are consistent; this one is just the widget-interface
slice.

## Open questions for the Phase 3 implementer

1. Does the two-level upcast (`IButton` -> `ITooltipped` -> `IElement`) work? The 40-cast collapse
   in answer A depends on it. If not, flatten `tooltip`/`disabled` into each interface.
2. Reading a `Lambda` field through an interface: clone or move? (E1.) Gate on the gallery running
   HeapAudit-clean twice.
3. Reading a `list<T>` field through an interface: lvalue or copy? (E2.)
4. Do the hosts keep calling `TabControl._title(i)` and `SplitView._ratioAt(x, y)`? Both are
   underscore-prefixed (private by convention) but reached from all/most hosts, so they must appear
   on the public interface - or the hosts must be refactored.
