const DOC_LIST = [
  {
    name: "README.md",
    title: "文档说明",
    desc: "插件文档入口、阅读指引与部署说明。",
  },
  {
    name: "MassBattleEditorMCP_Design.md",
    title: "插件设计与工具说明",
    desc: "接口设计、功能清单、工具边界与扩展要点。",
  },
  {
    name: "BatchFxMarketplaceConversion.md",
    title: "市场特效转批处理特效",
    desc: "把市场 FX 转为批处理 FX 的流程与注意点。",
  },
  {
    name: "BatchFxDemoLearning_JapanSoldier.md",
    title: "Demo 批处理实战",
    desc: "学习 Demo 爆炸、子弹、选择圈，并升级日本步兵。",
  },
  {
    name: "MassBattleEditor.md",
    title: "单位编辑与平衡实践",
    desc: "单位工作流、数值与表现字段的拆分与操作建议。",
  },
];

const toc = document.getElementById("toc");
const content = document.getElementById("docContent");
const title = document.getElementById("activeTitle");
const search = document.getElementById("docSearch");
const rawLink = document.getElementById("rawLink");
const errorTip = document.getElementById("errorTip");
const template = document.getElementById("docTemplate");

function setActive(file) {
  const links = toc.querySelectorAll(".doc-item");
  links.forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.file === file);
  });
}

function createNav(items) {
  const fragment = document.createDocumentFragment();
  items.forEach((item) => {
    const btn = template.content.firstElementChild.cloneNode(true);
    btn.textContent = `${item.title}`;
    btn.title = item.desc;
    btn.dataset.file = item.name;
    btn.addEventListener("click", () => loadDocument(item));
    fragment.appendChild(btn);
  });
  toc.appendChild(fragment);
}

function applySearch(keyword) {
  const list = DOC_LIST.filter((item) => item.title.includes(keyword) || item.desc.includes(keyword));
  toc.innerHTML = "";
  createNav(list);

  if (!list.length) {
    content.innerHTML = "<p>未命中文档，请换个关键词。</p>";
    rawLink.classList.add("hidden");
    title.textContent = "无结果";
    return;
  }
  const current = location.hash.replace("#", "");
  const active = list.find((item) => item.name === current) || list[0];
  loadDocument(active);
}

async function loadDocument(item) {
  if (!item) return;
  const selected = item.name || item;
  const found = DOC_LIST.find((entry) => entry.name === selected) || {
    name: item,
    title: item,
    desc: "文档",
  };

  location.hash = selected;
  title.textContent = found.title;
  rawLink.href = `./${encodeURIComponent(found.name)}`;
  rawLink.classList.remove("hidden");
  setActive(found.name);
  errorTip.classList.add("hidden");
  content.innerHTML = `<p>正在读取 <strong>${found.name}</strong>...</p>`;

  try {
    const res = await fetch(found.name);
    if (!res.ok) {
      throw new Error(`读取失败：${res.status} ${res.statusText}`);
    }
    const markdown = await res.text();
    content.innerHTML = window.marked.parse(markdown, {
      breaks: true,
      gfm: true,
    });
    content.querySelectorAll("pre code").forEach((block) => {
      window.hljs.highlightElement(block);
    });
  } catch (error) {
    content.innerHTML = `<p>无法加载 ${found.name}，请检查本地服务或网络。</p>`;
    errorTip.textContent = `${error.message}。建议启动本地服务后重新打开：python -m http.server 8080`;
    errorTip.classList.remove("hidden");
  }
}

function init() {
  createNav(DOC_LIST);
  const initial = DOC_LIST.find((item) => item.name === location.hash.replace("#", "")) || DOC_LIST[0];
  loadDocument(initial);

  search.addEventListener("input", (event) => {
    const keyword = (event.target.value || "").trim();
    applySearch(keyword);
  });
}

document.addEventListener("DOMContentLoaded", init);
