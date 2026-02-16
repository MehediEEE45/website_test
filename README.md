# GitHub Pages deployment

This repository contains a static monitoring dashboard (HTML/JS). The included GitHub Actions workflow deploys the repository root to the `gh-pages` branch whenever you push to `main`.

Quick steps:

1. Create a repository on GitHub and push this code to the `main` branch.
2. The workflow `.github/workflows/gh-pages.yml` will run and publish the site to the `gh-pages` branch.
3. In your repository Settings → Pages, set the Pages source to the `gh-pages` branch (if not auto-selected) and note the site URL.

Example push commands:

```bash
git init
git add .
git commit -m "Initial site"
git remote add origin <git-repo-URL>
git branch -M main
git push -u origin main
```

Notes:
- If you want a custom domain, add a `CNAME` file to the repository root containing your domain.
- The workflow uses the built-in `GITHUB_TOKEN` so no extra secrets are required.
- If your repository is named `username.github.io` then the site will be served at `https://username.github.io/`.
