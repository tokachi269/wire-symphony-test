# Wire cable visual

この文書はcable centerline、visual curve part、CableSection、span visual assembly、deformable cable asset materializationを所有する。topology/placementは[`state_and_persistence.md`](state_and_persistence.md)と[`generation.md`](generation.md)の決定済み出力を消費し、再判断しない。

## Cable curve

cable centerlineの正本はBezier制御点ではなく、attachment endpoint、gravity、sag、tangent policy、canonical direction、curve familyである。`domains/wire/src/geometry/curve`がこの意味入力からsample、arc length、frame、boundsを生成し、具体的な計算方式は`CurveMethod`で差し替える。

main spanの既定方式はparabolic sagとし、支持点でsag勾配を持つ実接線を維持する。端点微分が0になるdecorative offsetをmain cable centerlineへ使わない。中心線へ横揺れnoiseを入れない。continuity policyは端点接線とhandleを決めるが、main spanのsag profileを別方式へ切り替えない。

`CableTemplate.sag_factor`はspan全体に1回適用する単一のratioであり、start/endごとの加算値ではない。Endpoint constraintを経由するcurve方式でも、両端へ同じratioを重複適用してはならない。このratioが指定する物理sag量は`endpoint chord length * ratio`であり、span長、pass種別、continuity、曲げ剛性を理由に別倍率で再解釈しない。これらは端点接線やhandleを決めてもsag量を変更しない。

bundle lane、band、helix、noiseは安定したcenterlineとcanonical direction基準frameからvisual layerで展開する。G2接続は現時点の必須条件ではない。support/insulator leadとjumperはmain spanとは別のcurve familyとして扱い、未対応familyは別方式へsilent fallbackせず明示的に拒否する。

span-local attachment blend方式は採用しない。continuousな本線接続部を各span端に個別に押し込むとsample polyline上でG1が崩れやすく、main spanから接続部へ不自然に切り替わる。

## Visual curve parts

現在は派生debug/cacheとして`VisualCurvePart`を持ち、最小単位を`NodePatchCurve`と`EdgeBodyCurve`へ分ける。未接続のterminal endpointには`NodePatchCurve`を作らない。末端へ新しいedgeを延長した場合は、一意な未接続endpointが2つ揃った操作でcontinuityを記録し、通常角ならそのpairをpatchが消費する。branch追加後もmulti-incident全体を丸めず、continuityが明示するthrough 2-edgeだけを維持する。

node / bundle template / lane / 保存済みplacement band単位でpatchを分離し、位置近似やband再探索で接続を推測しない。main cable patchはattachmentを通過せず、incoming/outgoing boundary間をturn内側で単調に結ぶ1区間filletとする。境界では`EdgeBodyCurve`のparabolic sag実接線とG1接続する。attachmentは参照として保持し、insulator/clampへの接続は別`LeadCurve`が所有する。

`EdgeBodyCurve`は正式`CableCurve`とadaptive tessellationを共有する。attachmentは動かさず、boundaryはmain spanの外向き実接線を所定の水平距離まで延長した位置へ置く。短いspanでは水平距離をspan長の25%以下に制限する。branch自体やfixture境界は、明示的なfixture/lead/jumper仕様が入るまでpatchを推測しない。source-edge途中分岐のsource projectionは`SavedBackboneSpanBinding`から解決した派生curveを評価し、port間chordで補間しない。

`NodePatchCurve`と`EdgeBodyCurve`はtopology正本ではない。source node / edge / span / bundle / lane、boundary point、boundary tangentをdebug/captureで見えるようにするための派生出力である。描画やexport用に分割してもよいが、分割後のspan片が接続部curveのauthorityになってはいけない。長いrun全体を毎回正本として再計算する方式にはせず、dirty node + incident edge + 必要な1-hop程度の更新範囲に抑える。

scoped visual rebuildでは、`changed spans`、その端点でconnection visualを書き換える`affected nodes`、materializationのため読むだけの`context spans`を分ける。EdgeBody等のspan-owned partはchanged spanだけ、NodePatch/Jumper等のnode-owned partはaffected nodeだけを削除・置換する。context spanの反対側nodeは削除対象へ昇格させない。context不足時に既存connectionを削除してsilent skipすることは禁止する。

## CableInstance and CableSection

`Span`はtopology上の区間であり、描画上のcable identityではない。`CableInstance`と`CableSection`はvisual derive層の概念であり、`SavedBackboneGraph`、Span、Bundle、Portの正本を決めない。

population sectionは同じ`logical_span_id`に属するspan visual assembly memberである。base sectionとpopulation sectionsはそれぞれ既存のendpoint、sag、curve policyから通常のbody curveを作る。populationは`SavedBackboneGraph`、Span、Portを増やさず、配置不能時はomit diagnosticを残す。

旧cable populationのvisual-only duplicationをBundle本数のvariationとして使う経路は退役する。Bundle本数を`CableSectionLayout`の追加描画として生成せず、authoritative topologyが必要なBundleは通常のBundle / Port / Spanとして生成する。`population section`は1 logical span内のvisual assembly memberだけを意味する。

## Span visual assembly

span visual assemblyはderived visual outputであり、authoritative topologyではない。assemblyの単位とidentity ownerはsource logical span / Bundleであり、Bundleの全topology laneを物理的に束ねるものではない。1 logical spanは設定により1本または複数の近接visual memberとして描画できるが、Span、Port、Bundle、attachment、CableRun identityは増やさない。

membersはbase sectionと、そのlogical cableをつなぐNodePatch / Lead / Jumperから派生するvisual構成要素である。線種ごとの`SpanVisualAssemblyTemplate`が固定member数と基準間隔を所有する。logical cable centerlineは通常のsag curveを維持し、束全体または個別memberへ見た目だけのrandom wander、phase、twistを加えない。

main spanのsupport pathとmembersはhelixの内側に置き、support pathは内周上部に接し、membersは下側に配置する。helixはendpoint trim区間だけ生成し、電柱やattachmentへ接続しない。

visual memberの断面はcenter curveに直交するlateral/up 2次元平面へcompactに配置する。1本はcenter、2本は対向、3本は三角形、4本は正方形相当、5本以上は小さな決定的円形配置とし、packing solverは持たない。`visual_member_spacing_m`はmember中心間隔であり、実際の間隔はCableTemplateの外径以上にclampする。初期templateでは線径に小clearanceを加えたcompact配置とする。

基準断面offsetはendpointでも維持し、全memberをlogical endpointの1点へ収束させない。authoritative Port / logical endpointはvisual member endpoint群の重心であり、member数に応じてPort、Span、Bundle、attachment、fixtureを増やさない。

Communication、Optical、support path、helixは同じlogical-span assembly pipelineの設定差で表現する。別のedge-bundle groupingやcategory専用geometry ownerを持たず、geometry近傍からmemberを探索しない。main spanのsagは`ResolvedSpanCurveInputs.effective_sag_ratio`を最終curveまで一貫して使う。非HVは既存のhierarchical variationから小さな差を導出し、HVはvariation multiplierを適用せず従来値を維持する。

`BundleTemplate.span_visual_assembly`は固定visual member数・間隔、support、helixを含むassemblyの正本設定である。visual member数はauthoritative conductor数ではなく、Span、Port、Bundle、fixtureを増やさない。radiusが0の場合はsupport pathからのmember offset、member wire radius、helix wire radius、clearanceを含む最小半径をderived側で求める。contained memberはsupport pathのnormalized arc-length位置へ対応付け、helix内周から出ないように断面offsetをclampする。CommunicationとOpticalは同じcompact packingとcontainment処理を使う。Opticalのsupport path、member、helixは同じcenter pathとcontainment radiusを共有する。

NodePatch / Lead / Jumperもmain spanと同じBundle placement key、logical lane、compact cross-sectionを使い、接続区間だけcenter curve 1本へ戻さない。support pathとhelixはmain spanだけの補助表現であり、接続区間へ重複生成しない。明示radiusはsupport wireとhelix wireの径およびclearanceを収められない値を設定時に拒否する。

support pathはhelixと独立して有効化できる。全support pathはendpoint解決後に`make_primary_curve_between`で主曲線を1回構築する。`support_wire_pole_band_id == 0`はendpoint fixture socketまで解決済みのmember endpointを入力とし、endpoint trim区間で同じ接続点へ収束しながら中央部を線径分だけ離す。正数bandは明示band endpointを同じ主曲線生成へ入力する。

helixはどちらのsupport endpoint方式でも利用できる。既定OPTICALはband 0を使い、placement高さ変更時もsupport、contained member、helix断面を同じmember endpointから再導出する。HV/LV/Opticalでcurve familyを分岐せず、複数laneはlaneごとのspanに1本ずつ派生する。support-onlyではmember curveへcontainmentを適用しない。

helixはCableSectionではなく、logical span単位で派生するidentityを持たないvisual partである。memberの関連は`CableSectionKey.logical_span_id`による明示キーだけで解決し、別のCableSectionをcarrierとして保存する関係やgeometry近傍探索は持たない。assemblyはbase section、同じlogical spanのpopulation sections、support path、helixを一時的にまとめる。Bundleの複数laneを一つの束として扱わず、laneごとのlogical spanに独立して適用する。

## Render materialization boundary

Core visual generationはWire curve semanticsを所有する。sag済み`VisualCurvePart.samples`、part kind / supplemental kind、boundary/tangent、wire radius、material/color、helixのresolved axis/radiusを派生出力とする。Coreは具体GLB、asset family、authoring length、piece、variant、visual flipを知らない。

GLBのparseとglTF `LINES` primitiveの抽出、asset catalog/family mapping、実asset extentの計測、piece tiling、`partKey`由来のpresentation-only variantと長手軸180度flipはWeb asset adapter / render materializationが所有する。adapterはCoreの`VisualCurvePart.samples`をarc lengthで評価し、world-up基準frameへlocal X/Y/Zを機械的に写像する。sag、connection geometry、material、wire radiusを再判断せず、triangle faceのedgeをwire centerlineとして解釈しない。helixの局所巻き形状、半径を含むasset local offset、tessellationはGLBのauthored寸法をそのまま使い、Coreは配置axisとcontainmentのresolved radiusを決定する。`VisualModelInstance`はrigid model用のままとし、deformable cableをその経路へ押し込まない。

## Forbidden implementations

- 追加線やhelixを`SavedBackboneGraph` spanとして保存する。
- `CableSection`を第二のtopology正本にする。
- geometry proximityからmember、carrier、support pathを推測する。
- viewer側でmember groupingやcontinuityを補正する。
- `CableMember`、`CableMemberGroup`、`WrapCable`、`SupportWire`を新しい中心型として増やす。
