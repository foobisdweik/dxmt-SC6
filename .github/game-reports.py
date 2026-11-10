"""the wiki's Games page, from the Game reports discussions: game-reports.py <owner/repo>, GH_TOKEN in the environment"""
import json, os, re, sys, urllib.request

FIELDS = ["Game", "API", "Mullion", "Mac", "Wine", "Settings", "FPS"]
QUERY = """query($owner: String!, $name: String!, $category: ID!, $after: String) {
  repository(owner: $owner, name: $name) {
    discussions(first: 100, after: $after, categoryId: $category, orderBy: {field: CREATED_AT, direction: DESC}) {
      pageInfo { hasNextPage endCursor }
      nodes { number url body }
    }
  }
}"""
CATEGORY = """query($owner: String!, $name: String!) {
  repository(owner: $owner, name: $name) { discussionCategory(slug: "game-reports") { id } }
}"""


def graphql(query, variables):
    request = urllib.request.Request(
        "https://api.github.com/graphql",
        json.dumps({"query": query, "variables": variables}).encode(),
        {"Authorization": "bearer " + os.environ["GH_TOKEN"]},
    )
    answer = json.load(urllib.request.urlopen(request))
    if "errors" in answer:
        sys.exit(json.dumps(answer["errors"]))
    return answer["data"]["repository"]


def reports(repo):
    owner, name = repo.split("/")
    category = graphql(CATEGORY, {"owner": owner, "name": name})["discussionCategory"]["id"]
    after = None
    while True:
        page = graphql(QUERY, {"owner": owner, "name": name, "category": category, "after": after})["discussions"]
        yield from page["nodes"]
        if not page["pageInfo"]["hasNextPage"]:
            return
        after = page["pageInfo"]["endCursor"]


def fields(body):
    # a form's answers come as "### <label>" sections; an unanswered one reads "_No response_"
    found = {}
    for section in re.split(r"^### ", body, flags=re.M)[1:]:
        label, _, value = section.partition("\n")
        value = " ".join(value.split())
        found[label.strip()] = "" if value == "_No response_" else value.replace("|", "\\|")
    return found


def page(repo):
    rows = []
    for report in reports(repo):
        found = fields(report["body"])
        if all(found.get(f) for f in FIELDS):
            rows.append([found[f] for f in FIELDS] + [f"[#{report['number']}]({report['url']})"])
    rows.sort(key=lambda row: row[0].lower())
    lines = [
        "Games that run with Mullion, from the "
        f"[Game reports](https://github.com/{repo}/discussions/categories/game-reports). This page is generated, "
        "and a new report updates it.",
        "",
        "| " + " | ".join(FIELDS + ["Report"]) + " |",
        "|" + "---|" * (len(FIELDS) + 1),
    ]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.stdout.write(page(sys.argv[1]))
