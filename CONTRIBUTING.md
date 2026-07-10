# Collaboration Guide

## Members

The team uses neutral identifiers until GitHub accounts are confirmed:

- Member A
- Member B
- Member C

Roles rotate by sprint. No permanent team leader is assumed.

## Branches

- `main`: stable releases only
- `develop`: sprint integration
- `feature/sXX-a-*`: Member A task branch
- `feature/sXX-b-*`: Member B task branch
- `feature/sXX-c-*`: Member C task branch
- `fix/*`: bug fixes
- `release/*`: release preparation

## Required workflow

1. Move the matching project task to In Progress.
2. Create a feature branch from the latest `develop`.
3. Implement only the assigned task.
4. Add or update tests and documentation as required.
5. Commit with a meaningful message.
6. Push the branch and open a Pull Request to `develop`.
7. Request review from a different member.
8. Address review feedback and pass tests.
9. Merge the PR and mark the project task complete.

## Commit format

Use Conventional Commits:

```text
feat: add recursive directory scan
fix: reject backup target inside source
test: cover archive password failure
docs: describe manifest format
build: add MinGW build script
```

## Review rule

- Authors cannot approve their own Pull Requests.
- Each PR needs at least one review comment or approval.
- Do not commit generated binaries to feature branches.
- Do not rewrite shared history after a branch is reviewed.
