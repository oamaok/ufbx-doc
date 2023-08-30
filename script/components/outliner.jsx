import globalState from "./global-state"
import { h, Fragment } from "kaiku"
import { elementTypeCategory, typeToIconUrl } from "./common"

function TreeNode({ state, info, id, level=0 }) {
    const element = info.elements[id]
    const icon = typeToIconUrl(element.type)
    const padding = `${0.25+level}rem`

    let children = []
    if (element.type === "node") {
        if (state.outliner.showAttributes) children.push(...element.attribs)
        children.push(...element.children)
    } else if (element.type === "mesh") {
        if (state.outliner.showMaterials) children.push(...element.materials)
        if (state.outliner.showDeformers) children.push(...element.deformers)
    } else if (element.type === "material") {
        children = element.textures
    } else if (element.type === "skin_deformer") {
        children = element.clusters
    } else if (element.type === "blend_deformer") {
        children = element.channels
    } else if (element.type === "blend_channel") {
        children = element.keyframes
    }

    const onClick = () => {
        state.selectedElement = id
    }
    const onKeyPress = (e) => {
        if (e.code === "Space" || e.code === "Enter") {
            onClick()
        }
    }

    const category = elementTypeCategory[element.type]
    const catClass = `cat-${category}`
    const structName = `ufbx_${element.type}`

    return <li className="ol-node">
        <div
            className={() => ({
                "ol-row": true,
                "ol-selected": state.selectedElement === id,
                [catClass]: true,
            })}
            role="button"
            aria-label={`${element.type} ${element.name}`}
            tabIndex="0"
            style={{paddingLeft: padding}}
            onClick={onClick}
            onKeyPress={onKeyPress}
            >
            <img className="ol-icon" src={icon} title={structName} alt="" aria-hidden="true" />
            <span>{element.name}</span>
            <span className="ol-type">{element.type}</span>
        </div>
        <ul className="ol-list ol-nested">
            {children.map(c => <TreeNode state={state} info={info} id={c} level={level+1} />)}
        </ul>
    </li>
}

export default function Outliner({ id }) {
    const state = globalState.scenes[id]
    const info = globalState.infos[state.scene]
    if (!info) return null
    const rootId = info.rootNode
    return <div class="ol-container">
            <div className="ol-top">
            <ul className="ol-list" role="tree">{
                state.outliner.includeRoot ? (
                    <TreeNode state={state} info={info} id={rootId} />
                ) : (
                    info.elements[rootId].children.map(c => 
                        <TreeNode state={state} info={info} id={c} />)
                )
            }</ul>
        </div>
    </div>
}

